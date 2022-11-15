/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiAclTableManager.h"

#include "fboss/agent/gen-cpp2/switch_config_constants.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/CounterUtils.h"
#include "fboss/agent/hw/sai/api/AclApi.h"
#include "fboss/agent/hw/sai/store/SaiStore.h"
#include "fboss/agent/hw/sai/switch/SaiAclTableGroupManager.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiMirrorManager.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitch.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/agent/platforms/sai/SaiPlatform.h"

#include <folly/MacAddress.h>
#include <chrono>
#include <memory>

using namespace std::chrono;

namespace facebook::fboss {

sai_u32_range_t SaiAclTableManager::getFdbDstUserMetaDataRange() const {
  std::optional<SaiSwitchTraits::Attributes::FdbDstUserMetaDataRange> range =
      SaiSwitchTraits::Attributes::FdbDstUserMetaDataRange();
  return *(SaiApiTable::getInstance()->switchApi().getAttribute(
      managerTable_->switchManager().getSwitchSaiId(), range));
}

sai_u32_range_t SaiAclTableManager::getRouteDstUserMetaDataRange() const {
  std::optional<SaiSwitchTraits::Attributes::RouteDstUserMetaDataRange> range =
      SaiSwitchTraits::Attributes::RouteDstUserMetaDataRange();
  return *(SaiApiTable::getInstance()->switchApi().getAttribute(
      managerTable_->switchManager().getSwitchSaiId(), range));
}

sai_u32_range_t SaiAclTableManager::getNeighborDstUserMetaDataRange() const {
  std::optional<SaiSwitchTraits::Attributes::NeighborDstUserMetaDataRange>
      range = SaiSwitchTraits::Attributes::NeighborDstUserMetaDataRange();
  return *(SaiApiTable::getInstance()->switchApi().getAttribute(
      managerTable_->switchManager().getSwitchSaiId(), range));
}

sai_uint32_t SaiAclTableManager::getMetaDataMask(
    sai_uint32_t metaDataMax) const {
  /*
   * Round up to the next highest power of 2
   *
   * The idea is to set all the bits on the right hand side of the most
   * significant set bit to 1 and then increment the value by 1 so it 'rolls
   * over' to the nearest power of 2.
   *
   * Note, right shifting to power of 2 and OR'ing is enough - we don't need to
   * shift and OR by 3, 5, 6 etc. as shift + OR by (1, 2), (1, 4), (2, 4) etc.
   * already achieves that effect.
   *
   * Reference:
   * https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
   */

  // to handle the case when metaDataMax is already power of 2.
  metaDataMax--;
  std::array<int, 5> kNumBitsToShift = {1, 2, 4, 8, 16};
  for (auto numBitsToShift : kNumBitsToShift) {
    metaDataMax |= metaDataMax >> numBitsToShift;
  }
  metaDataMax++;

  // to handle the case when the passed metaDataMax is 0
  metaDataMax += (((metaDataMax == 0)) ? 1 : 0);

  return metaDataMax - 1;
}

std::vector<std::string> SaiAclTableManager::getAllHandleNames() const {
  std::vector<std::string> handleNames;
  for (const auto& [name, handle] : handles_) {
    handleNames.push_back(name);
  }
  return handleNames;
}

AclTableSaiId SaiAclTableManager::addAclTable(
    const std::shared_ptr<AclTable>& addedAclTable,
    cfg::AclStage aclStage) {
  auto saiAclStage =
      managerTable_->aclTableGroupManager().cfgAclStageToSaiAclStage(aclStage);

  /*
   * TODO(skhare)
   * Add single ACL Table for now (called during SaiSwitch::init()).
   * Later, extend SwitchState to carry AclTable, and then process it to
   * addAclTable.
   *
   * After ACL table is added, add it to appropriate ACL group:
   * managerTable_->switchManager().addTableGroupMember(SAI_ACL_STAGE_INGRESS,
   * aclTableSaiId);
   */

  // If we already store a handle for this this Acl Table, fail to add a new one
  auto aclTableName = addedAclTable->getID();
  auto handle = getAclTableHandle(aclTableName);
  if (handle) {
    throw FbossError("attempted to add a duplicate aclTable: ", aclTableName);
  }

  SaiAclTableTraits::AdapterHostKey adapterHostKey;
  SaiAclTableTraits::CreateAttributes attributes;

  std::tie(adapterHostKey, attributes) =
      aclTableCreateAttributes(saiAclStage, addedAclTable);

  auto& aclTableStore = saiStore_->get<SaiAclTableTraits>();
  std::shared_ptr<SaiAclTable> saiAclTable{};
  if (platform_->getHwSwitch()->getBootType() == BootType::WARM_BOOT) {
    if (auto existingAclTable = aclTableStore.get(adapterHostKey)) {
      if (attributes != existingAclTable->attributes() ||
          FLAGS_force_recreate_acl_tables) {
        auto key = existingAclTable->adapterHostKey();
        auto attrs = existingAclTable->attributes();
        existingAclTable = aclTableStore.setObject(key, attrs);
        recreateAclTable(existingAclTable, attributes);
      }
      saiAclTable = std::move(existingAclTable);
    }
  }
  saiAclTable = aclTableStore.setObject(adapterHostKey, attributes);
  auto aclTableHandle = std::make_unique<SaiAclTableHandle>();
  aclTableHandle->aclTable = saiAclTable;
  auto [it, inserted] =
      handles_.emplace(aclTableName, std::move(aclTableHandle));
  CHECK(inserted);

  auto aclTableSaiId = it->second->aclTable->adapterKey();

  // Add ACL Table to group based on the stage
  if (platform_->getAsic()->isSupported(HwAsic::Feature::ACL_TABLE_GROUP)) {
    managerTable_->aclTableGroupManager().addAclTableGroupMember(
        SAI_ACL_STAGE_INGRESS, aclTableSaiId, aclTableName);
  }

  return aclTableSaiId;
}

void SaiAclTableManager::removeAclTable(
    const std::shared_ptr<AclTable>& removedAclTable,
    cfg::AclStage /*aclStage*/) {
  auto aclTableName = removedAclTable->getID();

  // remove from acl table group
  if (hasTableGroups_) {
    managerTable_->aclTableGroupManager().removeAclTableGroupMember(
        SAI_ACL_STAGE_INGRESS, aclTableName);
  }

  // remove from handles
  handles_.erase(aclTableName);
}

void SaiAclTableManager::changedAclTable(
    const std::shared_ptr<AclTable>& oldAclTable,
    const std::shared_ptr<AclTable>& newAclTable,
    cfg::AclStage aclStage) {
  /*
   * TODO(skhare)
   * Extend SwitchState to carry AclTable, and then process it to change
   * AclTable.
   * (We would likely have to removeAclTable() and re addAclTable() due to ASIC
   * limitations.
   */

  /*
   * TODO(saranicholas)
   * Modify this to process acl entries delta, instead of removing and re adding
   */
  removeAclTable(oldAclTable, aclStage);
  addAclTable(newAclTable, aclStage);
}

const SaiAclTableHandle* FOLLY_NULLABLE
SaiAclTableManager::getAclTableHandle(const std::string& aclTableName) const {
  return getAclTableHandleImpl(aclTableName);
}

SaiAclTableHandle* FOLLY_NULLABLE
SaiAclTableManager::getAclTableHandle(const std::string& aclTableName) {
  return getAclTableHandleImpl(aclTableName);
}

SaiAclTableHandle* FOLLY_NULLABLE SaiAclTableManager::getAclTableHandleImpl(
    const std::string& aclTableName) const {
  auto itr = handles_.find(aclTableName);
  if (itr == handles_.end()) {
    return nullptr;
  }
  if (!itr->second || !itr->second->aclTable) {
    XLOG(FATAL) << "invalid null Acl table for: " << aclTableName;
  }
  return itr->second.get();
}

sai_uint32_t SaiAclTableManager::swPriorityToSaiPriority(int priority) const {
  /*
   * TODO(skhare)
   * When adding HwAclPriorityTests, add a test to verify that SAI
   * implementation treats larger value of priority as higher priority.
   * SwitchState: smaller ACL ID means higher priority.
   * BCM API: larger priority means higher priority.
   * BCM SAI: larger priority means higher priority.
   * Tajo SAI: larger priority means higher priority.
   * SAI spec: does not define?
   * But larger priority means higher priority is documented here:
   * https://github.com/opencomputeproject/SAI/blob/master/doc/SAI-Proposal-ACL-1.md
   */
  sai_uint32_t saiPriority = aclEntryMaximumPriority_ - priority;
  if (saiPriority < aclEntryMinimumPriority_) {
    throw FbossError(
        "Acl Entry priority out of range. Supported: [",
        aclEntryMinimumPriority_,
        ", ",
        aclEntryMaximumPriority_,
        "], specified: ",
        saiPriority);
  }

  return saiPriority;
}

sai_acl_ip_frag_t SaiAclTableManager::cfgIpFragToSaiIpFrag(
    cfg::IpFragMatch cfgType) const {
  switch (cfgType) {
    case cfg::IpFragMatch::MATCH_NOT_FRAGMENTED:
      return SAI_ACL_IP_FRAG_NON_FRAG;
    case cfg::IpFragMatch::MATCH_FIRST_FRAGMENT:
      return SAI_ACL_IP_FRAG_HEAD;
    case cfg::IpFragMatch::MATCH_NOT_FRAGMENTED_OR_FIRST_FRAGMENT:
      return SAI_ACL_IP_FRAG_NON_FRAG_OR_HEAD;
    case cfg::IpFragMatch::MATCH_NOT_FIRST_FRAGMENT:
      return SAI_ACL_IP_FRAG_NON_HEAD;
    case cfg::IpFragMatch::MATCH_ANY_FRAGMENT:
      return SAI_ACL_IP_FRAG_ANY;
  }
  // should return in one of the cases
  throw FbossError("Unsupported IP fragment option");
}

sai_acl_ip_type_t SaiAclTableManager::cfgIpTypeToSaiIpType(
    cfg::IpType cfgIpType) const {
  switch (cfgIpType) {
    case cfg::IpType::ANY:
      return SAI_ACL_IP_TYPE_ANY;
    case cfg::IpType::IP:
      return SAI_ACL_IP_TYPE_IP;
    case cfg::IpType::IP4:
      return SAI_ACL_IP_TYPE_IPV4ANY;
    case cfg::IpType::IP6:
      return SAI_ACL_IP_TYPE_IPV6ANY;
  }
  // should return in one of the cases
  throw FbossError("Unsupported IP Type option");
}

uint16_t SaiAclTableManager::cfgEtherTypeToSaiEtherType(
    cfg::EtherType cfgEtherType) const {
  switch (cfgEtherType) {
    case cfg::EtherType::ANY:
    case cfg::EtherType::IPv4:
    case cfg::EtherType::IPv6:
    case cfg::EtherType::EAPOL:
    case cfg::EtherType::MACSEC:
    case cfg::EtherType::LLDP:
      return static_cast<uint16_t>(cfgEtherType);
  }
  // should return in one of the cases
  throw FbossError("Unsupported EtherType option");
}

sai_uint32_t SaiAclTableManager::cfgLookupClassToSaiMetaDataAndMaskHelper(
    cfg::AclLookupClass lookupClass,
    sai_uint32_t dstUserMetaDataRangeMin,
    sai_uint32_t dstUserMetaDataRangeMax) const {
  auto dstUserMetaData = static_cast<int>(lookupClass);
  if (dstUserMetaData < dstUserMetaDataRangeMin ||
      dstUserMetaData > dstUserMetaDataRangeMax) {
    throw FbossError(
        "attempted to configure dstUserMeta larger than supported by this ASIC",
        dstUserMetaData,
        " supported min: ",
        dstUserMetaDataRangeMin,
        " max: ",
        dstUserMetaDataRangeMax);
  }

  return dstUserMetaData;
}

std::pair<sai_uint32_t, sai_uint32_t>
SaiAclTableManager::cfgLookupClassToSaiFdbMetaDataAndMask(
    cfg::AclLookupClass lookupClass) const {
  if (platform_->getAsic()->getAsicType() ==
      cfg::AsicType::ASIC_TYPE_TRIDENT2) {
    /*
     * lookupClassL2 is not configured on Trident2 or else the ASIC runs out
     * of resources. lookupClassL2 is needed for MH-NIC queue-per-host
     * solution. However, the solution is not applicable for Trident2 as we
     * don't implement queues on Trident2.
     */
    throw FbossError(
        "attempted to configure lookupClassL2 on Trident2, not needed/supported");
  }

  return std::make_pair(
      cfgLookupClassToSaiMetaDataAndMaskHelper(
          lookupClass,
          fdbDstUserMetaDataRangeMin_,
          fdbDstUserMetaDataRangeMax_),
      fdbDstUserMetaDataMask_);
}

std::pair<sai_uint32_t, sai_uint32_t>
SaiAclTableManager::cfgLookupClassToSaiRouteMetaDataAndMask(
    cfg::AclLookupClass lookupClass) const {
  return std::make_pair(
      cfgLookupClassToSaiMetaDataAndMaskHelper(
          lookupClass,
          routeDstUserMetaDataRangeMin_,
          routeDstUserMetaDataRangeMax_),
      routeDstUserMetaDataMask_);
}

std::pair<sai_uint32_t, sai_uint32_t>
SaiAclTableManager::cfgLookupClassToSaiNeighborMetaDataAndMask(
    cfg::AclLookupClass lookupClass) const {
  return std::make_pair(
      cfgLookupClassToSaiMetaDataAndMaskHelper(
          lookupClass,
          neighborDstUserMetaDataRangeMin_,
          neighborDstUserMetaDataRangeMax_),
      neighborDstUserMetaDataMask_);
}

std::vector<sai_int32_t>
SaiAclTableManager::cfgActionTypeListToSaiActionTypeList(
    const std::vector<cfg::AclTableActionType>& actionTypes) const {
  std::vector<sai_int32_t> saiActionTypeList;

  for (const auto& actionType : actionTypes) {
    sai_int32_t saiActionType;
    switch (actionType) {
      case cfg::AclTableActionType::PACKET_ACTION:
        saiActionType = SAI_ACL_ACTION_TYPE_PACKET_ACTION;
        break;
      case cfg::AclTableActionType::COUNTER:
        saiActionType = SAI_ACL_ACTION_TYPE_COUNTER;
        break;
      case cfg::AclTableActionType::SET_TC:
        saiActionType = SAI_ACL_ACTION_TYPE_SET_TC;
        break;
      case cfg::AclTableActionType::SET_DSCP:
        saiActionType = SAI_ACL_ACTION_TYPE_SET_DSCP;
        break;
      case cfg::AclTableActionType::MIRROR_INGRESS:
        saiActionType = SAI_ACL_ACTION_TYPE_MIRROR_INGRESS;
        break;
      case cfg::AclTableActionType::MIRROR_EGRESS:
        saiActionType = SAI_ACL_ACTION_TYPE_MIRROR_EGRESS;
        break;
      default:
        // should return in one of the cases
        throw FbossError("Unsupported Acl Table action type");
    }
    saiActionTypeList.push_back(saiActionType);
  }

  return saiActionTypeList;
}

bool isSameAclCounterAttributes(
    const SaiAclCounterTraits::CreateAttributes& fromStore,
    const SaiAclCounterTraits::CreateAttributes& fromSw) {
  return GET_ATTR(AclCounter, TableId, fromStore) ==
      GET_ATTR(AclCounter, TableId, fromSw) &&
#if SAI_API_VERSION >= SAI_VERSION(1, 10, 2)
      GET_OPT_ATTR(AclCounter, Label, fromStore) ==
      GET_OPT_ATTR(AclCounter, Label, fromSw) &&
#endif
      GET_OPT_ATTR(AclCounter, EnablePacketCount, fromStore) ==
      GET_OPT_ATTR(AclCounter, EnablePacketCount, fromSw) &&
      GET_OPT_ATTR(AclCounter, EnableByteCount, fromStore) ==
      GET_OPT_ATTR(AclCounter, EnableByteCount, fromSw);
}

std::pair<
    std::shared_ptr<SaiAclCounter>,
    std::vector<std::pair<cfg::CounterType, std::string>>>
SaiAclTableManager::addAclCounter(
    const SaiAclTableHandle* aclTableHandle,
    const cfg::TrafficCounter& trafficCount,
    const SaiAclEntryTraits::AdapterHostKey& aclEntryAdapterHostKey) {
  std::vector<std::pair<cfg::CounterType, std::string>> aclCounterTypeAndName;

  SaiAclCounterTraits::Attributes::TableId aclTableId{
      aclTableHandle->aclTable->adapterKey()};

#if SAI_API_VERSION >= SAI_VERSION(1, 10, 2)
  SaiCharArray32 counterLabel{};
  if ((*trafficCount.name()).size() > 31) {
    throw FbossError(
        "ACL Counter Label:",
        *trafficCount.name(),
        " size ",
        (*trafficCount.name()).size(),
        " exceeds max(31)");
  }

  std::copy(
      (*trafficCount.name()).begin(),
      (*trafficCount.name()).end(),
      counterLabel.begin());

  std::optional<SaiAclCounterTraits::Attributes::Label> aclCounterLabel{
      counterLabel};
#endif

  std::optional<SaiAclCounterTraits::Attributes::EnablePacketCount>
      enablePacketCount{false};
  std::optional<SaiAclCounterTraits::Attributes::EnableByteCount>
      enableByteCount{false};

  for (const auto& counterType : *trafficCount.types()) {
    std::string statSuffix;
    switch (counterType) {
      case cfg::CounterType::PACKETS:
        enablePacketCount =
            SaiAclCounterTraits::Attributes::EnablePacketCount{true};
        statSuffix = "packets";
        break;
      case cfg::CounterType::BYTES:
        enableByteCount =
            SaiAclCounterTraits::Attributes::EnableByteCount{true};
        statSuffix = "bytes";
        break;
      default:
        throw FbossError("Unsupported CounterType for ACL");
    }

    auto statName =
        folly::to<std::string>(*trafficCount.name(), ".", statSuffix);
    aclCounterTypeAndName.push_back(std::make_pair(counterType, statName));
    aclStats_.reinitStat(statName, std::nullopt);
  }

  SaiAclCounterTraits::AdapterHostKey adapterHostKey {
    aclTableId,
#if SAI_API_VERSION >= SAI_VERSION(1, 10, 2)
        aclCounterLabel,
#endif
        enablePacketCount, enableByteCount,
  };

  SaiAclCounterTraits::CreateAttributes attributes {
    aclTableId,
#if SAI_API_VERSION >= SAI_VERSION(1, 10, 2)
        aclCounterLabel,
#endif
        enablePacketCount, enableByteCount,
        std::nullopt, // counterPackets
        std::nullopt, // counterBytes
  };

  // The following logic is added temporarily for 5.1 -> 7.2 warmboot
  // transition. Sai spec 1.10.2 introduces label attributes for acl
  // counters, which requires agent detaching the old acl counter without
  // label attributes from the acl entry and then reattach the new acl
  // counter. In order to attach the new counter, agent needs to set counter
  // attribute to SAI_NULL_OBJECT_ID, and then attach the newly created
  // counter. Following is the logic to check if the attach/detach logic is
  // needed.
  // TODO(zecheng): remove the following logic when 7.2 upgrade completes.
  auto& aclCounterStore = saiStore_->get<SaiAclCounterTraits>();
  auto& aclEntryStore = saiStore_->get<SaiAclEntryTraits>();
  // If acl entry exists and already has different acl counter attached to
  // it, detach the old acl counter and then attach the new one.
  if (auto existingAclEntry = aclEntryStore.get(aclEntryAdapterHostKey)) {
    auto oldActionCounter = AclCounterSaiId{
        GET_OPT_ATTR(AclEntry, ActionCounter, existingAclEntry->attributes())
            .getData()};
    if (auto existingAclCounter = aclCounterStore.find(oldActionCounter)) {
      if (!isSameAclCounterAttributes(
              existingAclCounter->attributes(), attributes)) {
        // Detach the old one
        SaiAclEntryTraits::Attributes::ActionCounter actionCounterAttribute{
            SAI_NULL_OBJECT_ID};
        SaiApiTable::getInstance()->aclApi().setAttribute(
            existingAclEntry->adapterKey(), actionCounterAttribute);
      }
    }
  }
  auto saiAclCounter = aclCounterStore.setObject(adapterHostKey, attributes);

  return std::make_pair(saiAclCounter, aclCounterTypeAndName);
}

AclEntrySaiId SaiAclTableManager::addAclEntry(
    const std::shared_ptr<AclEntry>& addedAclEntry,
    const std::string& aclTableName) {
  // If we attempt to add entry to a table that does not exist, fail.
  auto aclTableHandle = getAclTableHandle(aclTableName);
  if (!aclTableHandle) {
    throw FbossError(
        "attempted to add AclEntry to a AclTable that does not exist: ",
        aclTableName);
  }

  // If we already store a handle for this this Acl Entry, fail to add new one.
  auto aclEntryHandle =
      getAclEntryHandle(aclTableHandle, addedAclEntry->getPriority());
  if (aclEntryHandle) {
    throw FbossError(
        "attempted to add a duplicate aclEntry: ", addedAclEntry->getID());
  }

  auto& aclEntryStore = saiStore_->get<SaiAclEntryTraits>();

  SaiAclEntryTraits::Attributes::TableId aclTableId{
      aclTableHandle->aclTable->adapterKey()};
  SaiAclEntryTraits::Attributes::Priority priority{
      swPriorityToSaiPriority(addedAclEntry->getPriority())};
  SaiAclEntryTraits::AdapterHostKey adapterHostKey{aclTableId, priority};

  std::optional<SaiAclEntryTraits::Attributes::FieldSrcIpV6> fieldSrcIpV6{
      std::nullopt};
  std::optional<SaiAclEntryTraits::Attributes::FieldSrcIpV4> fieldSrcIpV4{
      std::nullopt};
  if (addedAclEntry->getSrcIp().first) {
    if (addedAclEntry->getSrcIp().first.isV6()) {
      auto srcIpV6Mask = folly::IPAddressV6(
          folly::IPAddressV6::fetchMask(addedAclEntry->getSrcIp().second));
      fieldSrcIpV6 = SaiAclEntryTraits::Attributes::FieldSrcIpV6{
          AclEntryFieldIpV6(std::make_pair(
              addedAclEntry->getSrcIp().first.asV6(), srcIpV6Mask))};
    } else if (addedAclEntry->getSrcIp().first.isV4()) {
      auto srcIpV4Mask = folly::IPAddressV4(
          folly::IPAddressV4::fetchMask(addedAclEntry->getSrcIp().second));
      fieldSrcIpV4 = SaiAclEntryTraits::Attributes::FieldSrcIpV4{
          AclEntryFieldIpV4(std::make_pair(
              addedAclEntry->getSrcIp().first.asV4(), srcIpV4Mask))};
    }
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldDstIpV6> fieldDstIpV6{
      std::nullopt};
  std::optional<SaiAclEntryTraits::Attributes::FieldDstIpV4> fieldDstIpV4{
      std::nullopt};
  if (addedAclEntry->getDstIp().first) {
    if (addedAclEntry->getDstIp().first.isV6()) {
      auto dstIpV6Mask = folly::IPAddressV6(
          folly::IPAddressV6::fetchMask(addedAclEntry->getDstIp().second));
      fieldDstIpV6 = SaiAclEntryTraits::Attributes::FieldDstIpV6{
          AclEntryFieldIpV6(std::make_pair(
              addedAclEntry->getDstIp().first.asV6(), dstIpV6Mask))};
    } else if (addedAclEntry->getDstIp().first.isV4()) {
      auto dstIpV4Mask = folly::IPAddressV4(
          folly::IPAddressV4::fetchMask(addedAclEntry->getDstIp().second));
      fieldDstIpV4 = SaiAclEntryTraits::Attributes::FieldDstIpV4{
          AclEntryFieldIpV4(std::make_pair(
              addedAclEntry->getDstIp().first.asV4(), dstIpV4Mask))};
    }
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldSrcPort> fieldSrcPort{
      std::nullopt};
  if (addedAclEntry->getSrcPort()) {
    if (addedAclEntry->getSrcPort().value() !=
        cfg::switch_config_constants::CPU_PORT_LOGICALID()) {
      auto portHandle = managerTable_->portManager().getPortHandle(
          PortID(addedAclEntry->getSrcPort().value()));
      if (!portHandle) {
        throw FbossError(
            "attempted to configure srcPort: ",
            addedAclEntry->getSrcPort().value(),
            " ACL:",
            addedAclEntry->getID());
      }
      fieldSrcPort =
          SaiAclEntryTraits::Attributes::FieldSrcPort{AclEntryFieldSaiObjectIdT(
              std::make_pair(portHandle->port->adapterKey(), kMaskDontCare))};
    } else {
      fieldSrcPort = SaiAclEntryTraits::Attributes::FieldSrcPort{
          AclEntryFieldSaiObjectIdT(std::make_pair(
              managerTable_->switchManager().getCpuPort(), kMaskDontCare))};
    }
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldOutPort> fieldOutPort{
      std::nullopt};
  if (addedAclEntry->getDstPort()) {
    auto portHandle = managerTable_->portManager().getPortHandle(
        PortID(addedAclEntry->getDstPort().value()));
    if (!portHandle) {
      throw FbossError(
          "attempted to configure dstPort: ",
          addedAclEntry->getDstPort().value(),
          " ACL:",
          addedAclEntry->getID());
    }
    fieldOutPort =
        SaiAclEntryTraits::Attributes::FieldOutPort{AclEntryFieldSaiObjectIdT(
            std::make_pair(portHandle->port->adapterKey(), kMaskDontCare))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldL4SrcPort> fieldL4SrcPort{
      std::nullopt};
  if (addedAclEntry->getL4SrcPort()) {
    fieldL4SrcPort = SaiAclEntryTraits::Attributes::FieldL4SrcPort{
        AclEntryFieldU16(std::make_pair(
            addedAclEntry->getL4SrcPort().value(), kL4PortMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldL4DstPort> fieldL4DstPort{
      std::nullopt};
  if (addedAclEntry->getL4DstPort()) {
    fieldL4DstPort = SaiAclEntryTraits::Attributes::FieldL4DstPort{
        AclEntryFieldU16(std::make_pair(
            addedAclEntry->getL4DstPort().value(), kL4PortMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldIpProtocol> fieldIpProtocol{
      std::nullopt};
  if (addedAclEntry->getProto()) {
    fieldIpProtocol = SaiAclEntryTraits::Attributes::FieldIpProtocol{
        AclEntryFieldU8(std::make_pair(
            addedAclEntry->getProto().value(), kIpProtocolMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldTcpFlags> fieldTcpFlags{
      std::nullopt};
  if (addedAclEntry->getTcpFlagsBitMap()) {
    fieldTcpFlags = SaiAclEntryTraits::Attributes::FieldTcpFlags{
        AclEntryFieldU8(std::make_pair(
            addedAclEntry->getTcpFlagsBitMap().value(), kTcpFlagsMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldIpFrag> fieldIpFrag{
      std::nullopt};
  if (addedAclEntry->getIpFrag()) {
    auto ipFragData = cfgIpFragToSaiIpFrag(addedAclEntry->getIpFrag().value());
    fieldIpFrag = SaiAclEntryTraits::Attributes::FieldIpFrag{
        AclEntryFieldU32(std::make_pair(ipFragData, kMaskDontCare))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldIcmpV4Type> fieldIcmpV4Type{
      std::nullopt};
  std::optional<SaiAclEntryTraits::Attributes::FieldIcmpV4Code> fieldIcmpV4Code{
      std::nullopt};
  std::optional<SaiAclEntryTraits::Attributes::FieldIcmpV6Type> fieldIcmpV6Type{
      std::nullopt};
  std::optional<SaiAclEntryTraits::Attributes::FieldIcmpV6Code> fieldIcmpV6Code{
      std::nullopt};
  if (addedAclEntry->getIcmpType()) {
    if (addedAclEntry->getProto()) {
      if (addedAclEntry->getProto().value() == AclEntryFields::kProtoIcmp) {
        fieldIcmpV4Type = SaiAclEntryTraits::Attributes::FieldIcmpV4Type{
            AclEntryFieldU8(std::make_pair(
                addedAclEntry->getIcmpType().value(), kIcmpTypeMask))};
        if (addedAclEntry->getIcmpCode()) {
          fieldIcmpV4Code = SaiAclEntryTraits::Attributes::FieldIcmpV4Code{
              AclEntryFieldU8(std::make_pair(
                  addedAclEntry->getIcmpCode().value(), kIcmpCodeMask))};
        }
      } else if (
          addedAclEntry->getProto().value() == AclEntryFields::kProtoIcmpv6) {
        fieldIcmpV6Type = SaiAclEntryTraits::Attributes::FieldIcmpV6Type{
            AclEntryFieldU8(std::make_pair(
                addedAclEntry->getIcmpType().value(), kIcmpTypeMask))};
        if (addedAclEntry->getIcmpCode()) {
          fieldIcmpV6Code = SaiAclEntryTraits::Attributes::FieldIcmpV6Code{
              AclEntryFieldU8(std::make_pair(
                  addedAclEntry->getIcmpCode().value(), kIcmpCodeMask))};
        }
      }
    }
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldDscp> fieldDscp{
      std::nullopt};
  if (addedAclEntry->getDscp()) {
    fieldDscp = SaiAclEntryTraits::Attributes::FieldDscp{AclEntryFieldU8(
        std::make_pair(addedAclEntry->getDscp().value(), kDscpMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldDstMac> fieldDstMac{
      std::nullopt};
  if (addedAclEntry->getDstMac()) {
    fieldDstMac = SaiAclEntryTraits::Attributes::FieldDstMac{AclEntryFieldMac(
        std::make_pair(addedAclEntry->getDstMac().value(), kMacMask()))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldIpType> fieldIpType{
      std::nullopt};
  if (addedAclEntry->getIpType()) {
    auto ipTypeData = cfgIpTypeToSaiIpType(addedAclEntry->getIpType().value());
    fieldIpType = SaiAclEntryTraits::Attributes::FieldIpType{
        AclEntryFieldU32(std::make_pair(ipTypeData, kMaskDontCare))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldTtl> fieldTtl{std::nullopt};
  if (addedAclEntry->getTtl()) {
    fieldTtl =
        SaiAclEntryTraits::Attributes::FieldTtl{AclEntryFieldU8(std::make_pair(
            addedAclEntry->getTtl().value().getValue(),
            addedAclEntry->getTtl().value().getMask()))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldRouteDstUserMeta>
      fieldRouteDstUserMeta{std::nullopt};
  if (addedAclEntry->getLookupClassRoute()) {
    fieldRouteDstUserMeta =
        SaiAclEntryTraits::Attributes::FieldRouteDstUserMeta{
            AclEntryFieldU32(cfgLookupClassToSaiRouteMetaDataAndMask(
                addedAclEntry->getLookupClassRoute().value()))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldNeighborDstUserMeta>
      fieldNeighborDstUserMeta{std::nullopt};
  if (addedAclEntry->getLookupClassNeighbor()) {
    fieldNeighborDstUserMeta =
        SaiAclEntryTraits::Attributes::FieldNeighborDstUserMeta{
            AclEntryFieldU32(cfgLookupClassToSaiNeighborMetaDataAndMask(
                addedAclEntry->getLookupClassNeighbor().value()))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldEthertype> fieldEtherType{
      std::nullopt};
  if (addedAclEntry->getEtherType()) {
    auto etherTypeData =
        cfgEtherTypeToSaiEtherType(addedAclEntry->getEtherType().value());
    fieldEtherType = SaiAclEntryTraits::Attributes::FieldEthertype{
        AclEntryFieldU16(std::make_pair(etherTypeData, kEtherTypeMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldOuterVlanId>
      fieldOuterVlanId{std::nullopt};
  if (addedAclEntry->getVlanID()) {
    fieldOuterVlanId = SaiAclEntryTraits::Attributes::FieldOuterVlanId{
        AclEntryFieldU16(std::make_pair(
            addedAclEntry->getVlanID().value(), kOuterVlanIdMask))};
  }

  std::optional<SaiAclEntryTraits::Attributes::FieldFdbDstUserMeta>
      fieldFdbDstUserMeta{std::nullopt};
  if (addedAclEntry->getLookupClassL2()) {
    fieldFdbDstUserMeta = SaiAclEntryTraits::Attributes::FieldFdbDstUserMeta{
        AclEntryFieldU32(cfgLookupClassToSaiFdbMetaDataAndMask(
            addedAclEntry->getLookupClassL2().value()))};
  }

  // TODO(skhare) Support all other ACL actions
  std::optional<SaiAclEntryTraits::Attributes::ActionPacketAction>
      aclActionPacketAction{std::nullopt};
  const auto& act = addedAclEntry->getActionType();
  if (act == cfg::AclActionType::DENY) {
    aclActionPacketAction = SaiAclEntryTraits::Attributes::ActionPacketAction{
        SAI_PACKET_ACTION_DROP};
  } else {
    aclActionPacketAction = SaiAclEntryTraits::Attributes::ActionPacketAction{
        SAI_PACKET_ACTION_FORWARD};
  }

  std::shared_ptr<SaiAclCounter> saiAclCounter{nullptr};
  std::vector<std::pair<cfg::CounterType, std::string>> aclCounterTypeAndName;
  std::optional<SaiAclEntryTraits::Attributes::ActionCounter> aclActionCounter{
      std::nullopt};

  std::optional<SaiAclEntryTraits::Attributes::ActionSetTC> aclActionSetTC{
      std::nullopt};

  std::optional<SaiAclEntryTraits::Attributes::ActionSetDSCP> aclActionSetDSCP{
      std::nullopt};

  std::optional<SaiAclEntryTraits::Attributes::ActionMirrorIngress>
      aclActionMirrorIngress{};

  std::optional<SaiAclEntryTraits::Attributes::ActionMirrorEgress>
      aclActionMirrorEgress{};

  std::optional<std::string> ingressMirror{std::nullopt};
  std::optional<std::string> egressMirror{std::nullopt};

  std::optional<SaiAclEntryTraits::Attributes::ActionMacsecFlow>
      aclActionMacsecFlow{std::nullopt};

  auto action = addedAclEntry->getAclAction();
  if (action) {
    if (action.value().getTrafficCounter()) {
      std::tie(saiAclCounter, aclCounterTypeAndName) = addAclCounter(
          aclTableHandle,
          action.value().getTrafficCounter().value(),
          adapterHostKey);
      aclActionCounter = SaiAclEntryTraits::Attributes::ActionCounter{
          AclEntryActionSaiObjectIdT(
              AclCounterSaiId{saiAclCounter->adapterKey()})};
    }

    if (action.value().getSendToQueue()) {
      auto sendToQueue = action.value().getSendToQueue().value();
      bool sendToCpu = sendToQueue.second;
      if (!sendToCpu) {
        auto queueId = static_cast<sai_uint8_t>(*sendToQueue.first.queueId());
        aclActionSetTC = SaiAclEntryTraits::Attributes::ActionSetTC{
            AclEntryActionU8(queueId)};
      } else {
        /*
         * When sendToCpu is set, a copy of the packet will be sent
         * to CPU.
         * By default, these packets are sent to queue 0.
         * Set TC to set the right traffic class which
         * will be mapped to queue id.
         *
         * TODO(skhare)
         * By default, BCM maps TC i to Queue i for i in [0, 9].
         * Tajo claims to map TC i to Queue i by default as well.
         * However, explicitly set the QoS Map and associate with the CPU port.
         */

        auto setCopyOrTrap = [&aclActionPacketAction, &aclActionSetTC](
                                 const MatchAction::SendToQueue& sendToQueue,
                                 sai_uint32_t packetAction) {
          aclActionPacketAction =
              SaiAclEntryTraits::Attributes::ActionPacketAction{packetAction};

          auto queueId = static_cast<sai_uint8_t>(*sendToQueue.first.queueId());
          aclActionSetTC = SaiAclEntryTraits::Attributes::ActionSetTC{
              AclEntryActionU8(queueId)};
        };

        if (action.value().getToCpuAction()) {
          switch (action.value().getToCpuAction().value()) {
            case cfg::ToCpuAction::COPY:
              if (!platform_->getAsic()->isSupported(
                      HwAsic::Feature::ACL_COPY_TO_CPU)) {
                throw FbossError("COPY_TO_CPU is not supported on this ASIC");
              }

              setCopyOrTrap(sendToQueue, SAI_PACKET_ACTION_COPY);
              break;
            case cfg::ToCpuAction::TRAP:
              setCopyOrTrap(sendToQueue, SAI_PACKET_ACTION_TRAP);
              break;
          }
        }
      }
    }

    if (action.value().getIngressMirror().has_value()) {
      std::vector<sai_object_id_t> aclEntryMirrorIngressOidList;
      auto mirrorHandle = managerTable_->mirrorManager().getMirrorHandle(
          action.value().getIngressMirror().value());
      if (mirrorHandle) {
        aclEntryMirrorIngressOidList.push_back(mirrorHandle->adapterKey());
      }
      ingressMirror = action.value().getIngressMirror().value();
      aclActionMirrorIngress =
          SaiAclEntryTraits::Attributes::ActionMirrorIngress{
              AclEntryActionSaiObjectIdList(aclEntryMirrorIngressOidList)};
    }

    if (action.value().getEgressMirror().has_value()) {
      std::vector<sai_object_id_t> aclEntryMirrorEgressOidList;
      auto mirrorHandle = managerTable_->mirrorManager().getMirrorHandle(
          action.value().getEgressMirror().value());
      if (mirrorHandle) {
        aclEntryMirrorEgressOidList.push_back(mirrorHandle->adapterKey());
      }
      egressMirror = action.value().getEgressMirror().value();
      aclActionMirrorEgress = SaiAclEntryTraits::Attributes::ActionMirrorEgress{
          AclEntryActionSaiObjectIdList(aclEntryMirrorEgressOidList)};
    }

    if (action.value().getSetDscp()) {
      const int dscpValue = *action.value().getSetDscp().value().dscpValue();

      aclActionSetDSCP = SaiAclEntryTraits::Attributes::ActionSetDSCP{
          AclEntryActionU8(dscpValue)};
    }

    if (action.value().getMacsecFlow()) {
      auto macsecFlowAction = action.value().getMacsecFlow().value();
      if (*macsecFlowAction.action() ==
          cfg::MacsecFlowPacketAction::MACSEC_FLOW) {
        sai_object_id_t flowId =
            static_cast<sai_object_id_t>(*macsecFlowAction.flowId());
        aclActionMacsecFlow = SaiAclEntryTraits::Attributes::ActionMacsecFlow{
            AclEntryActionSaiObjectIdT(flowId)};
      } else if (
          *macsecFlowAction.action() == cfg::MacsecFlowPacketAction::FORWARD) {
        aclActionPacketAction =
            SaiAclEntryTraits::Attributes::ActionPacketAction{
                SAI_PACKET_ACTION_FORWARD};
      } else if (
          *macsecFlowAction.action() == cfg::MacsecFlowPacketAction::DROP) {
        aclActionPacketAction =
            SaiAclEntryTraits::Attributes::ActionPacketAction{
                SAI_PACKET_ACTION_DROP};
      } else {
        throw FbossError(
            "Unsupported Macsec Flow action for ACL entry: ",
            addedAclEntry->getID(),
            " Macsec Flow action ",
            apache::thrift::util::enumNameSafe(*macsecFlowAction.action()));
      }
    }
  }

  // TODO(skhare) At least one field and one action must be specified.
  // Once we add support for all fields and actions, throw error if that is not
  // honored.
  auto matcherIsValid =
      (fieldSrcIpV6.has_value() || fieldDstIpV6.has_value() ||
       fieldSrcIpV4.has_value() || fieldDstIpV4.has_value() ||
       fieldSrcPort.has_value() || fieldOutPort.has_value() ||
       fieldL4SrcPort.has_value() || fieldL4DstPort.has_value() ||
       fieldIpProtocol.has_value() || fieldTcpFlags.has_value() ||
       fieldIpFrag.has_value() || fieldIcmpV4Type.has_value() ||
       fieldIcmpV4Code.has_value() || fieldIcmpV6Type.has_value() ||
       fieldIcmpV6Code.has_value() || fieldDscp.has_value() ||
       fieldDstMac.has_value() || fieldIpType.has_value() ||
       fieldTtl.has_value() || fieldFdbDstUserMeta.has_value() ||
       fieldRouteDstUserMeta.has_value() || fieldEtherType.has_value() ||
       fieldNeighborDstUserMeta.has_value() ||
       platform_->getAsic()->isSupported(HwAsic::Feature::EMPTY_ACL_MATCHER));
  if (fieldSrcPort.has_value()) {
    matcherIsValid &= platform_->getAsic()->isSupported(
        HwAsic::Feature::SAI_ACL_ENTRY_SRC_PORT_QUALIFIER);
  }
  auto actionIsValid =
      (aclActionPacketAction.has_value() || aclActionCounter.has_value() ||
       aclActionSetTC.has_value() || aclActionSetDSCP.has_value() ||
       aclActionMirrorIngress.has_value() ||
       aclActionMirrorEgress.has_value() || aclActionMacsecFlow.has_value());

  if (!(matcherIsValid && actionIsValid)) {
    XLOG(WARNING) << "Unsupported field/action for aclEntry: "
                  << addedAclEntry->getID() << " MactherValid "
                  << ((matcherIsValid) ? "true" : "false") << " ActionValid "
                  << ((actionIsValid) ? "true" : "false");
    return AclEntrySaiId{0};
  }

  SaiAclEntryTraits::CreateAttributes attributes{
      aclTableId,
      priority,
      true,
      fieldSrcIpV6,
      fieldDstIpV6,
      fieldSrcIpV4,
      fieldDstIpV4,
      fieldSrcPort,
      fieldOutPort,
      fieldL4SrcPort,
      fieldL4DstPort,
      fieldIpProtocol,
      fieldTcpFlags,
      fieldIpFrag,
      fieldIcmpV4Type,
      fieldIcmpV4Code,
      fieldIcmpV6Type,
      fieldIcmpV6Code,
      fieldDscp,
      fieldDstMac,
      fieldIpType,
      fieldTtl,
      fieldFdbDstUserMeta,
      fieldRouteDstUserMeta,
      fieldNeighborDstUserMeta,
      fieldEtherType,
      fieldOuterVlanId,
      aclActionPacketAction,
      aclActionCounter,
      aclActionSetTC,
      aclActionSetDSCP,
      aclActionMirrorIngress,
      aclActionMirrorEgress,
      aclActionMacsecFlow,
  };

  auto saiAclEntry = aclEntryStore.setObject(adapterHostKey, attributes);
  auto entryHandle = std::make_unique<SaiAclEntryHandle>();
  entryHandle->aclEntry = saiAclEntry;
  entryHandle->aclCounter = saiAclCounter;
  entryHandle->aclCounterTypeAndName = aclCounterTypeAndName;
  entryHandle->ingressMirror = ingressMirror;
  entryHandle->egressMirror = egressMirror;
  auto [it, inserted] = aclTableHandle->aclTableMembers.emplace(
      addedAclEntry->getPriority(), std::move(entryHandle));
  CHECK(inserted);

  XLOG(DBG2) << "added acl entry " << addedAclEntry->getID() << " priority "
             << addedAclEntry->getPriority();

  auto enabled = SaiApiTable::getInstance()->aclApi().getAttribute(
      it->second->aclEntry->adapterKey(),
      SaiAclEntryTraits::Attributes::Enabled{});
  CHECK(enabled) << "Acl entry: " << addedAclEntry->getID() << " not enabled";
  return it->second->aclEntry->adapterKey();
}

void SaiAclTableManager::removeAclEntry(
    const std::shared_ptr<AclEntry>& removedAclEntry,
    const std::string& aclTableName) {
  // If we attempt to remove entry for a table that does not exist, fail.
  auto aclTableHandle = getAclTableHandle(aclTableName);
  if (!aclTableHandle) {
    throw FbossError(
        "attempted to remove AclEntry to a AclTable that does not exist: ",
        aclTableName);
  }

  // If we attempt to remove entry that does not exist, fail.
  auto itr =
      aclTableHandle->aclTableMembers.find(removedAclEntry->getPriority());
  if (itr == aclTableHandle->aclTableMembers.end()) {
    // an acl entry that uses cpu port as qualifier may not have been created
    // even if it exists in switch state.
    XLOG(ERR) << "attempted to remove aclEntry which does not exist: ",
        removedAclEntry->getID();
    return;
  }

  aclTableHandle->aclTableMembers.erase(itr);

  auto action = removedAclEntry->getAclAction();
  if (action && action.value().getTrafficCounter()) {
    removeAclCounter(action.value().getTrafficCounter().value());
  }
  XLOG(DBG2) << "removed acl  entry " << removedAclEntry->getID()
             << " priority " << removedAclEntry->getPriority();
}

void SaiAclTableManager::removeAclCounter(
    const cfg::TrafficCounter& trafficCount) {
  for (const auto& counterType : *trafficCount.types()) {
    auto statName =
        utility::statNameFromCounterType(*trafficCount.name(), counterType);
    aclStats_.removeStat(statName);
  }
}

void SaiAclTableManager::changedAclEntry(
    const std::shared_ptr<AclEntry>& oldAclEntry,
    const std::shared_ptr<AclEntry>& newAclEntry,
    const std::string& aclTableName) {
  /*
   * ASIC/SAI implementation typically does not allow modifying an ACL entry.
   * Thus, remove and re-add.
   */
  XLOG(DBG2) << "changing acl entry " << oldAclEntry->getID();
  removeAclEntry(oldAclEntry, aclTableName);
  addAclEntry(newAclEntry, aclTableName);
}

const SaiAclEntryHandle* FOLLY_NULLABLE SaiAclTableManager::getAclEntryHandle(
    const SaiAclTableHandle* aclTableHandle,
    int priority) const {
  auto itr = aclTableHandle->aclTableMembers.find(priority);
  if (itr == aclTableHandle->aclTableMembers.end()) {
    return nullptr;
  }
  if (!itr->second || !itr->second->aclEntry) {
    XLOG(FATAL) << "invalid null Acl entry for: " << priority;
  }
  return itr->second.get();
}

void SaiAclTableManager::programMirror(
    const SaiAclEntryHandle* aclEntryHandle,
    MirrorDirection direction,
    MirrorAction action,
    const std::optional<std::string>& mirrorId) {
  if (!mirrorId.has_value()) {
    XLOG(DBG) << "mirror session not configured: ";
    return;
  }

  std::vector<sai_object_id_t> mirrorOidList{};
  if (action == MirrorAction::START) {
    auto mirrorHandle =
        managerTable_->mirrorManager().getMirrorHandle(mirrorId.value());
    if (mirrorHandle) {
      mirrorOidList.push_back(mirrorHandle->adapterKey());
    }
  }

  if (direction == MirrorDirection::INGRESS) {
    aclEntryHandle->aclEntry->setOptionalAttribute(
        SaiAclEntryTraits::Attributes::ActionMirrorIngress{mirrorOidList});
  } else {
    aclEntryHandle->aclEntry->setOptionalAttribute(
        SaiAclEntryTraits::Attributes::ActionMirrorEgress{mirrorOidList});
  }
}

void SaiAclTableManager::programMirrorOnAllAcls(
    const std::optional<std::string>& mirrorId,
    MirrorAction action) {
  for (const auto& handle : handles_) {
    for (const auto& aclMember : handle.second->aclTableMembers) {
      if (aclMember.second->getIngressMirror() == mirrorId) {
        programMirror(
            aclMember.second.get(), MirrorDirection::INGRESS, action, mirrorId);
      }
      if (aclMember.second->getEgressMirror() == mirrorId) {
        programMirror(
            aclMember.second.get(), MirrorDirection::EGRESS, action, mirrorId);
      }
    }
  }
}

void SaiAclTableManager::removeUnclaimedAclEntries() {
  auto& aclEntryStore = saiStore_->get<SaiAclEntryTraits>();
  aclEntryStore.removeUnexpectedUnclaimedWarmbootHandles();
}

void SaiAclTableManager::updateStats() {
  auto now = duration_cast<seconds>(system_clock::now().time_since_epoch());

  for (const auto& handle : handles_) {
    for (const auto& aclMember : handle.second->aclTableMembers) {
      for (const auto& [counterType, counterName] :
           aclMember.second->aclCounterTypeAndName) {
        switch (counterType) {
          case cfg::CounterType::PACKETS: {
            auto counterPackets =
                SaiApiTable::getInstance()->aclApi().getAttribute(
                    aclMember.second->aclCounter->adapterKey(),
                    SaiAclCounterTraits::Attributes::CounterPackets());
            aclStats_.updateStat(now, counterName, counterPackets);
          } break;
          case cfg::CounterType::BYTES: {
            auto counterBytes =
                SaiApiTable::getInstance()->aclApi().getAttribute(
                    aclMember.second->aclCounter->adapterKey(),
                    SaiAclCounterTraits::Attributes::CounterBytes());
            aclStats_.updateStat(now, counterName, counterBytes);
          } break;
          default:
            throw FbossError("Unsupported CounterType for ACL");
        }
      }
    }
  }
}

std::set<cfg::AclTableQualifier> SaiAclTableManager::getSupportedQualifierSet()
    const {
  /*
   * Not all the qualifiers are supported by every ASIC.
   * Moreover, different ASICs have different max key widths.
   * Thus, enabling all the supported qualifiers in the same ACL Table could
   * overflow the max key width.
   *
   * Thus, only enable a susbet of supported qualifiers based on the ASIC
   * capability.
   */
  bool isTajo = platform_->getAsic()->getAsicVendor() ==
      HwAsic::AsicVendor::ASIC_VENDOR_TAJO;
  bool isTrident2 =
      platform_->getAsic()->getAsicType() == cfg::AsicType::ASIC_TYPE_TRIDENT2;

  if (isTajo) {
    std::set<cfg::AclTableQualifier> tajoQualifiers = {
        cfg::AclTableQualifier::SRC_IPV6,
        cfg::AclTableQualifier::DST_IPV6,
        cfg::AclTableQualifier::SRC_IPV4,
        cfg::AclTableQualifier::DST_IPV4,
        cfg::AclTableQualifier::IP_PROTOCOL,
        cfg::AclTableQualifier::DSCP,
        cfg::AclTableQualifier::IP_TYPE,
        cfg::AclTableQualifier::TTL,
        cfg::AclTableQualifier::LOOKUP_CLASS_L2,
        cfg::AclTableQualifier::LOOKUP_CLASS_NEIGHBOR,
        cfg::AclTableQualifier::LOOKUP_CLASS_ROUTE};

    return tajoQualifiers;
  } else {
    std::set<cfg::AclTableQualifier> bcmQualifiers = {
        cfg::AclTableQualifier::SRC_IPV6,
        cfg::AclTableQualifier::DST_IPV6,
        cfg::AclTableQualifier::SRC_IPV4,
        cfg::AclTableQualifier::DST_IPV4,
        cfg::AclTableQualifier::L4_SRC_PORT,
        cfg::AclTableQualifier::L4_DST_PORT,
        cfg::AclTableQualifier::IP_PROTOCOL,
        cfg::AclTableQualifier::TCP_FLAGS,
        cfg::AclTableQualifier::SRC_PORT,
        cfg::AclTableQualifier::OUT_PORT,
        cfg::AclTableQualifier::IP_FRAG,
        cfg::AclTableQualifier::ICMPV4_TYPE,
        cfg::AclTableQualifier::ICMPV4_CODE,
        cfg::AclTableQualifier::ICMPV6_TYPE,
        cfg::AclTableQualifier::ICMPV6_CODE,
        cfg::AclTableQualifier::DSCP,
        cfg::AclTableQualifier::DST_MAC,
        cfg::AclTableQualifier::IP_TYPE,
        cfg::AclTableQualifier::TTL,
        cfg::AclTableQualifier::LOOKUP_CLASS_L2,
        cfg::AclTableQualifier::LOOKUP_CLASS_NEIGHBOR,
        cfg::AclTableQualifier::LOOKUP_CLASS_ROUTE};

    /*
     * FdbDstUserMetaData is required only for MH-NIC queue-per-host solution.
     * However, the solution is not applicable for Trident2 as FBOSS does not
     * implement queues on Trident2.
     * Furthermore, Trident2 supports fewer ACL qualifiers than other
     * hardwares. Thus, avoid programming unncessary qualifiers (or else we
     * run out resources).
     */
    if (isTrident2) {
      bcmQualifiers.erase(cfg::AclTableQualifier::LOOKUP_CLASS_L2);
    }

    return bcmQualifiers;
  }
}

void SaiAclTableManager::addDefaultAclTable() {
  if (handles_.find(kAclTable1) != handles_.end()) {
    throw FbossError("default acl table already exists.");
  }
  auto table1 = std::make_shared<AclTable>(
      0,
      kAclTable1); // TODO(saranicholas): set appropriate table priority
  addAclTable(table1, cfg::AclStage::INGRESS);
}

void SaiAclTableManager::removeDefaultAclTable() {
  if (handles_.find(kAclTable1) == handles_.end()) {
    return;
  }
  // remove from acl table group
  if (platform_->getAsic()->isSupported(HwAsic::Feature::ACL_TABLE_GROUP)) {
    managerTable_->aclTableGroupManager().removeAclTableGroupMember(
        SAI_ACL_STAGE_INGRESS, kAclTable1);
  }
  handles_.erase(kAclTable1);
}

bool SaiAclTableManager::isQualifierSupported(
    const std::string& aclTableName,
    cfg::AclTableQualifier qualifier) const {
  auto handle = getAclTableHandle(aclTableName);
  if (!handle) {
    throw FbossError("ACL table ", aclTableName, " not found.");
  }
  auto attributes = handle->aclTable->attributes();

  auto hasField = [attributes](auto field) {
    if (!field) {
      return false;
    }
    return field->value();
  };

  using cfg::AclTableQualifier;
  switch (qualifier) {
    case cfg::AclTableQualifier::SRC_IPV6:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldSrcIpV6>>(
              attributes));
    case cfg::AclTableQualifier::DST_IPV6:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldDstIpV6>>(
              attributes));
    case cfg::AclTableQualifier::SRC_IPV4:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldDstIpV4>>(
              attributes));
    case cfg::AclTableQualifier::DST_IPV4:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldSrcIpV4>>(
              attributes));
    case cfg::AclTableQualifier::L4_SRC_PORT:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldL4SrcPort>>(
              attributes));
    case cfg::AclTableQualifier::L4_DST_PORT:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldL4DstPort>>(
              attributes));
    case cfg::AclTableQualifier::IP_PROTOCOL:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldIpProtocol>>(
              attributes));
    case cfg::AclTableQualifier::TCP_FLAGS:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldTcpFlags>>(
              attributes));
    case cfg::AclTableQualifier::SRC_PORT:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldSrcPort>>(
              attributes));
    case cfg::AclTableQualifier::OUT_PORT:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldOutPort>>(
              attributes));
    case cfg::AclTableQualifier::IP_FRAG:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldIpFrag>>(
              attributes));
    case cfg::AclTableQualifier::ICMPV4_TYPE:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldIcmpV4Type>>(
              attributes));
    case cfg::AclTableQualifier::ICMPV4_CODE:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldIcmpV4Code>>(
              attributes));
    case cfg::AclTableQualifier::ICMPV6_TYPE:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldIcmpV6Type>>(
              attributes));
    case cfg::AclTableQualifier::ICMPV6_CODE:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldIcmpV6Code>>(
              attributes));
    case cfg::AclTableQualifier::DSCP:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldDscp>>(
              attributes));
    case cfg::AclTableQualifier::DST_MAC:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldDstMac>>(
              attributes));
    case cfg::AclTableQualifier::IP_TYPE:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldIpType>>(
              attributes));
    case cfg::AclTableQualifier::TTL:
      return hasField(
          std::get<std::optional<SaiAclTableTraits::Attributes::FieldTtl>>(
              attributes));
    case cfg::AclTableQualifier::LOOKUP_CLASS_L2:
      return hasField(
          std::get<std::optional<
              SaiAclTableTraits::Attributes::FieldFdbDstUserMeta>>(attributes));
    case cfg::AclTableQualifier::LOOKUP_CLASS_NEIGHBOR:
      return hasField(
          std::get<std::optional<
              SaiAclTableTraits::Attributes::FieldRouteDstUserMeta>>(
              attributes));
    case cfg::AclTableQualifier::LOOKUP_CLASS_ROUTE:
      return hasField(
          std::get<std::optional<
              SaiAclTableTraits::Attributes::FieldRouteDstUserMeta>>(
              attributes));
    case cfg::AclTableQualifier::ETHER_TYPE:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldEthertype>>(
              attributes));
    case cfg::AclTableQualifier::OUTER_VLAN:
      return hasField(
          std::get<
              std::optional<SaiAclTableTraits::Attributes::FieldOuterVlanId>>(
              attributes));
  }
  return false;
}

bool SaiAclTableManager::areQualifiersSupported(
    const std::string& aclTableName,
    const std::set<cfg::AclTableQualifier>& qualifiers) const {
  return std::all_of(
      std::begin(qualifiers),
      std::end(qualifiers),
      [this, aclTableName](auto qualifier) {
        return isQualifierSupported(aclTableName, qualifier);
      });
}

bool SaiAclTableManager::areQualifiersSupportedInDefaultAclTable(
    const std::set<cfg::AclTableQualifier>& qualifiers) const {
  return areQualifiersSupported(kAclTable1, qualifiers);
}

void SaiAclTableManager::recreateAclTable(
    std::shared_ptr<SaiAclTable>& aclTable,
    const SaiAclTableTraits::CreateAttributes& newAttributes) {
  if (!platform_->getAsic()->isSupported(
          HwAsic::Feature::SAI_ACL_TABLE_UPDATE)) {
    XLOG(WARNING) << "feature to update acl table is not supported";
    return;
  }
  XLOG(DBG2) << "refreshing acl table schema";
  auto adapterHostKey = aclTable->adapterHostKey();
  auto& aclEntryStore = saiStore_->get<SaiAclEntryTraits>();

  std::map<
      SaiAclEntryTraits::AdapterHostKey,
      SaiAclEntryTraits::CreateAttributes>
      entries{};
  // remove acl entries from acl table, retain their attributes
  for (const auto& entry : aclEntryStore) {
    auto key = entry.second.lock()->adapterHostKey();
    if (std::get<SaiAclEntryTraits::Attributes::TableId>(key) !=
        static_cast<sai_object_id_t>(aclTable->adapterKey())) {
      continue;
    }
    auto value = entry.second.lock()->attributes();
    auto aclEntry = aclEntryStore.setObject(key, value);
    entries.emplace(key, value);
    aclEntry.reset();
  }
  // remove group member and acl table, since store holds only weak ptr after
  // setObject is invoked, clearing returned shared ptr is enough to destroy SAI
  // object and call SAI remove API.
  std::shared_ptr<SaiAclTableGroupMember> groupMember{};
  SaiAclTableGroupMemberTraits::AdapterHostKey memberAdapterHostKey{};
  SaiAclTableGroupMemberTraits::CreateAttributes memberAttrs{};
  auto& aclGroupMemberStore = saiStore_->get<SaiAclTableGroupMemberTraits>();
  sai_object_id_t aclTableGroupId{};
  if (platform_->getAsic()->isSupported(HwAsic::Feature::ACL_TABLE_GROUP)) {
    for (auto entry : aclGroupMemberStore) {
      auto member = entry.second.lock();
      auto key = member->adapterHostKey();
      auto attrs = member->attributes();
      if (std::get<SaiAclTableGroupMemberTraits::Attributes::TableId>(attrs) !=
          static_cast<sai_object_id_t>(aclTable->adapterKey())) {
        continue;
      }
      groupMember = aclGroupMemberStore.setObject(key, attrs);
      memberAdapterHostKey = groupMember->adapterHostKey();
      memberAttrs = groupMember->attributes();
      break;
    }
    aclTableGroupId =
        std::get<SaiAclTableGroupMemberTraits::Attributes::TableGroupId>(
            memberAttrs)
            .value();
    managerTable_->switchManager().resetIngressAcl();
    // reset group member
    groupMember.reset();
  }
  // remove acl table
  aclTable.reset();

  // update acl table
  auto& aclTableStore = saiStore_->get<SaiAclTableTraits>();
  aclTable = aclTableStore.setObject(adapterHostKey, newAttributes);
  // restore acl table group member

  sai_object_id_t tableId = aclTable->adapterKey();
  if (platform_->getAsic()->isSupported(HwAsic::Feature::ACL_TABLE_GROUP)) {
    std::get<SaiAclTableGroupMemberTraits::Attributes::TableId>(
        memberAdapterHostKey) = tableId;
    std::get<SaiAclTableGroupMemberTraits::Attributes::TableId>(memberAttrs) =
        tableId;
    aclGroupMemberStore.addWarmbootHandle(memberAdapterHostKey, memberAttrs);
    managerTable_->switchManager().setIngressAcl(aclTableGroupId);
  }
  // skip recreating acl entries as acl entry information is lost.
  // this happens because SAI API layer returns default values for unset ACL
  // entry attributes. some of the attributes may be unsupported in sdk or could
  // stretch the key width beyind what's supported.
}

void SaiAclTableManager::removeUnclaimedAclCounter() {
  saiStore_->get<SaiAclCounterTraits>().removeUnclaimedWarmbootHandlesIf(
      [](const auto& aclCounter) {
        aclCounter->release();
        return true;
      });
}

} // namespace facebook::fboss
