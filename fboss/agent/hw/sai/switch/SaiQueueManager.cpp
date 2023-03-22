/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/sai/switch/SaiQueueManager.h"
#include <fboss/agent/hw/sai/api/QueueApi.h>
#include "fboss/agent/platforms/sai/SaiPlatform.h"

#include "fboss/agent/hw/sai/store/SaiStore.h"
#include "fboss/agent/hw/sai/switch/SaiManagerTable.h"
#include "fboss/agent/hw/sai/switch/SaiPortManager.h"
#include "fboss/agent/hw/sai/switch/SaiSwitchManager.h"
#include "fboss/agent/hw/switch_asics/HwAsic.h"
#include "fboss/lib/TupleUtils.h"

namespace facebook::fboss {

namespace {

void fillHwQueueStats(
    uint8_t queueId,
    const folly::F14FastMap<sai_stat_id_t, uint64_t>& counterId2Value,
    HwPortStats& hwPortStats) {
  for (auto counterIdAndValue : counterId2Value) {
    auto [counterId, value] = counterIdAndValue;
    switch (counterId) {
      case SAI_QUEUE_STAT_PACKETS:
        hwPortStats.queueOutPackets_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_BYTES:
        hwPortStats.queueOutBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_DROPPED_BYTES:
        hwPortStats.queueOutDiscardBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_DROPPED_PACKETS:
        hwPortStats.queueOutDiscardPackets_()[queueId] = value;
        /*
         * Out congestion packets on a port is a sum of all queue
         * out discards on a port
         */
        hwPortStats.outCongestionDiscardPkts_() =
            *hwPortStats.outCongestionDiscardPkts_() + value;
        break;
      case SAI_QUEUE_STAT_WATERMARK_BYTES:
        hwPortStats.queueWatermarkBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_WATERMARK_LEVEL:
        hwPortStats.queueWatermarkLevel_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_WRED_DROPPED_PACKETS:
        hwPortStats.queueWredDroppedPackets_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_WRED_ECN_MARKED_PACKETS:
        hwPortStats.queueEcnMarkedPackets_()[queueId] = value;
        break;
      default:
        throw FbossError("Got unexpected queue counter id: ", counterId);
    }
  }
}
void fillHwQueueStats(
    uint8_t queueId,
    const folly::F14FastMap<sai_stat_id_t, uint64_t>& counterId2Value,
    HwSysPortStats& hwSysPortStats) {
  for (auto counterIdAndValue : counterId2Value) {
    auto [counterId, value] = counterIdAndValue;
    switch (counterId) {
      case SAI_QUEUE_STAT_BYTES:
        hwSysPortStats.queueOutBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_DROPPED_BYTES:
        hwSysPortStats.queueOutDiscardBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_WATERMARK_BYTES:
        hwSysPortStats.queueWatermarkBytes_()[queueId] = value;
        break;
      case SAI_QUEUE_STAT_WRED_DROPPED_PACKETS:
        hwSysPortStats.queueWredDroppedPackets_()[queueId] = value;
        break;
      default:
        throw FbossError("Got unexpected queue counter id: ", counterId);
    }
  }
}
} // namespace

namespace detail {

SaiQueueTraits::CreateAttributes makeQueueAttributes(
    PortSaiId portSaiId,
    const PortQueue& portQueue) {
  sai_queue_type_t type;
  switch (portQueue.getStreamType()) {
    case cfg::StreamType::UNICAST:
      type = SAI_QUEUE_TYPE_UNICAST;
      break;
    case cfg::StreamType::MULTICAST:
      type = SAI_QUEUE_TYPE_MULTICAST;
      break;
    case cfg::StreamType::ALL:
      type = SAI_QUEUE_TYPE_ALL;
      break;
    case cfg::StreamType::FABRIC_TX:
      type = SAI_QUEUE_TYPE_FABRIC_TX;
      break;
  }
  return SaiQueueTraits::CreateAttributes{
      type,
      portSaiId,
      portQueue.getID(),
      portSaiId,
      std::nullopt,
      std::nullopt,
      std::nullopt};
}

SaiQueueConfig makeSaiQueueConfig(
    const SaiQueueTraits::AdapterHostKey& adapterHostKey) {
  auto queueIndex = std::get<SaiQueueTraits::Attributes::Index>(adapterHostKey);
  auto queueType = std::get<SaiQueueTraits::Attributes::Type>(adapterHostKey);
  cfg::StreamType streamType;
  switch (queueType.value()) {
    case SAI_QUEUE_TYPE_UNICAST:
    case SAI_QUEUE_TYPE_UNICAST_VOQ:
      streamType = cfg::StreamType::UNICAST;
      break;
    case SAI_QUEUE_TYPE_MULTICAST:
    case SAI_QUEUE_TYPE_MULTICAST_VOQ:
      streamType = cfg::StreamType::MULTICAST;
      break;
    case SAI_QUEUE_TYPE_ALL:
      streamType = cfg::StreamType::ALL;
      break;
    case SAI_QUEUE_TYPE_FABRIC_TX:
      streamType = cfg::StreamType::FABRIC_TX;
      break;
    default:
      throw FbossError("Unhandled SAI queue type: ", queueType.value());
  }
  return std::make_pair(queueIndex.value(), streamType);
}
} // namespace detail

void SaiQueueHandle::resetQueue() {
  /*
   * Queues cannot be deleted as it is owned by the adapter but all
   * the queue attributes has to be reset to default. As a
   * temporary solution, resetting the queue attributes to their
   * defaults. For long term, this will be removed and resetting the
   * objects will be part of SaiObject.
   */

  queue->setOptionalAttribute(
      SaiQueueTraits::Attributes::SchedulerProfileId{SAI_NULL_OBJECT_ID});
  if (wredProfile) {
    queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::WredProfileId{SAI_NULL_OBJECT_ID});
  }
  if (bufferProfile) {
    queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::BufferProfileId{SAI_NULL_OBJECT_ID});
  }
}

SaiQueueHandle::~SaiQueueHandle() {
  resetQueue();
}

SaiQueueManager::SaiQueueManager(
    SaiStore* saiStore,
    SaiManagerTable* managerTable,
    const SaiPlatform* platform)
    : saiStore_(saiStore), managerTable_(managerTable), platform_(platform) {}

void SaiQueueManager::changeQueueEcnWred(
    SaiQueueHandle* queueHandle,
    const PortQueue& newPortQueue) {
  auto qType = SaiApiTable::getInstance()->queueApi().getAttribute(
      queueHandle->queue->adapterKey(), SaiQueueTraits::Attributes::Type{});
  if (platform_->getAsic()->isSupported(HwAsic::Feature::VOQ) &&
      (SAI_QUEUE_TYPE_UNICAST_VOQ != qType) &&
      (SAI_QUEUE_TYPE_MULTICAST_VOQ != qType)) {
    // VOQ switches support WRED/ECN configs on voqs only
    return;
  }
  auto newWredProfile =
      managerTable_->wredManager().getOrCreateProfile(newPortQueue);
  if (newWredProfile != queueHandle->wredProfile) {
    queueHandle->queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::WredProfileId(
            newWredProfile ? newWredProfile->adapterKey()
                           : SAI_NULL_OBJECT_ID));
    queueHandle->wredProfile = newWredProfile;
  }
}

void SaiQueueManager::changeQueueBufferProfile(
    SaiQueueHandle* queueHandle,
    const PortQueue& newPortQueue) {
  auto newBufferProfile =
      managerTable_->bufferManager().getOrCreateProfile(newPortQueue);
  if (newBufferProfile != queueHandle->bufferProfile) {
    queueHandle->queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::BufferProfileId(
            newBufferProfile ? newBufferProfile->adapterKey()
                             : SAI_NULL_OBJECT_ID));
    queueHandle->bufferProfile = newBufferProfile;
  }
}

void SaiQueueManager::changeQueueScheduler(
    SaiQueueHandle* queueHandle,
    const PortQueue& newPortQueue) {
  std::shared_ptr<SaiScheduler> newScheduler;
  if (newPortQueue.getScheduling() != cfg::QueueScheduling::INTERNAL) {
    newScheduler =
        managerTable_->schedulerManager().createScheduler(newPortQueue);
  }

  if (newScheduler != queueHandle->scheduler) {
    queueHandle->queue->setOptionalAttribute(
        SaiQueueTraits::Attributes::SchedulerProfileId(
            newScheduler ? newScheduler->adapterKey() : SAI_NULL_OBJECT_ID));
    // Update scheduler reference after we have set the queue
    // scheduler attribute, else if this is the last queue
    // referring to this scheduler, we will try to delete it
    // before we have updated the SAI reference.
    queueHandle->scheduler = newScheduler;
  }
}

void SaiQueueManager::changeQueue(
    SaiQueueHandle* queueHandle,
    const PortQueue& newPortQueue) {
  CHECK(queueHandle);
  auto queueType = GET_ATTR(Queue, Type, queueHandle->queue->attributes());
  changeQueueScheduler(queueHandle, newPortQueue);
  if (platform_->getAsic()->isSupported(HwAsic::Feature::SAI_ECN_WRED)) {
    changeQueueEcnWred(queueHandle, newPortQueue);
  }
  if (platform_->getAsic()->isSupported(HwAsic::Feature::BUFFER_POOL) &&
      (queueType != SAI_QUEUE_TYPE_UNICAST_VOQ) &&
      (queueType != SAI_QUEUE_TYPE_MULTICAST_VOQ) &&
      (queueType != SAI_QUEUE_TYPE_FABRIC_TX)) {
    changeQueueBufferProfile(queueHandle, newPortQueue);
  }
}

void SaiQueueManager::ensurePortQueueConfig(
    PortSaiId portSaiId,
    const SaiQueueHandles& queueHandles,
    const QueueConfig& queues) {
  for (const auto& portQueue : queues) {
    SaiQueueTraits::CreateAttributes attributes =
        detail::makeQueueAttributes(portSaiId, *portQueue);
    SaiQueueTraits::AdapterHostKey adapterHostKey = tupleProjection<
        SaiQueueTraits::CreateAttributes,
        SaiQueueTraits::AdapterHostKey>(attributes);
    auto& store = saiStore_->get<SaiQueueTraits>();
    auto queue = store.get(adapterHostKey);
    if (!queue) {
      throw FbossError(
          "failed to find queue in store for queue id: ", portQueue->getID());
    }
    SaiQueueConfig queueConfig =
        std::make_pair(portQueue->getID(), portQueue->getStreamType());
    auto queueHandleEntry = queueHandles.find(queueConfig);
    if (queueHandleEntry == queueHandles.end()) {
      throw FbossError(
          "failed to find queue handle for queue id: ", (*portQueue).getID());
    }
    changeQueue(queueHandleEntry->second.get(), *portQueue);
  }
}

SaiQueueHandles SaiQueueManager::loadQueues(
    const std::vector<QueueSaiId>& queueSaiIds) {
  SaiQueueHandles queueHandles;
  auto& store = saiStore_->get<SaiQueueTraits>();
  for (auto queueSaiId : queueSaiIds) {
    auto queueHandle = std::make_unique<SaiQueueHandle>(
        store.loadObjectOwnedByAdapter(queueSaiId));
    store.loadObjectOwnedByAdapter(SaiQueueTraits::AdapterKey{queueSaiId});
    auto saiQueueConfig =
        detail::makeSaiQueueConfig(queueHandle->queue->adapterHostKey());
    queueHandles[saiQueueConfig] = std::move(queueHandle);
  }
  return queueHandles;
}

const std::vector<sai_stat_id_t>&
SaiQueueManager::supportedNonWatermarkCounterIdsRead(
    int queueType,
    SaiQueueHandle* queueHandle) const {
  if (queueType == SAI_QUEUE_TYPE_MULTICAST_VOQ ||
      queueType == SAI_QUEUE_TYPE_UNICAST_VOQ) {
    return voqNonWatermarkCounterIdsRead(queueType, queueHandle);
  } else if (queueType == SAI_QUEUE_TYPE_FABRIC_TX) {
    static const std::vector<sai_stat_id_t> kFabricQueueNonWatermarksStats;
    return kFabricQueueNonWatermarksStats;
  }
  return egressQueueNonWatermarkCounterIdsRead(queueType);
}

const std::vector<sai_stat_id_t>&
SaiQueueManager::voqNonWatermarkCounterIdsRead(
    int /*queueType*/,
    SaiQueueHandle* queueHandle) const {
  assert(queueHandle);
  static std::vector<sai_stat_id_t> baseCounterIds(
      SaiQueueTraits::VoqNonWatermarkCounterIdsToRead.begin(),
      SaiQueueTraits::VoqNonWatermarkCounterIdsToRead.end());
  static std::vector<sai_stat_id_t> basePlusWredCounterIds;
  if (!basePlusWredCounterIds.size()) {
    basePlusWredCounterIds.resize(
        baseCounterIds.size() + SaiQueueTraits::WredCounterIdsToRead.size());
    std::set_union(
        baseCounterIds.begin(),
        baseCounterIds.end(),
        SaiQueueTraits::WredCounterIdsToRead.begin(),
        SaiQueueTraits::WredCounterIdsToRead.end(),
        basePlusWredCounterIds.begin());
  }

  if (queueHandle && queueHandle->wredProfile &&
      GET_ATTR(Wred, GreenEnable, queueHandle->wredProfile->attributes())) {
    return basePlusWredCounterIds;
  }
  return baseCounterIds;
}

const std::vector<sai_stat_id_t>&
SaiQueueManager::egressQueueNonWatermarkCounterIdsRead(int queueType) const {
  static std::vector<sai_stat_id_t> baseCounterIds(
      SaiQueueTraits::NonWatermarkCounterIdsToRead.begin(),
      SaiQueueTraits::NonWatermarkCounterIdsToRead.end());

  /*
   * Per-queue WRED discard counters are not supported for
   * multicast queues in Broadcom platforms.
   * So return baseCounterIds in that case, or if SAI_ECN_WRED
   * feature is unsupported by the HwAsic.
   */
  if (((queueType == SAI_QUEUE_TYPE_MULTICAST) &&
       (platform_->getAsic()->getAsicVendor() ==
        HwAsic::AsicVendor::ASIC_VENDOR_BCM)) ||
      !platform_->getAsic()->isSupported(HwAsic::Feature::SAI_ECN_WRED)) {
    return baseCounterIds;
  }

  /*
   * All platforms supporting SAI_ECN_WRED have WRED counters.
   */
  static std::vector<sai_stat_id_t> extendedCounterIds{};
  if (extendedCounterIds.size() == 0) {
    extendedCounterIds.insert(
        extendedCounterIds.end(), baseCounterIds.begin(), baseCounterIds.end());

    extendedCounterIds.insert(
        extendedCounterIds.end(),
        SaiQueueTraits::NonWatermarkWredCounterIdsToRead.begin(),
        SaiQueueTraits::NonWatermarkWredCounterIdsToRead.end());

    if (platform_->getAsic()->isSupported(HwAsic::Feature::QUEUE_ECN_COUNTER)) {
      // Supported on TAJO only with SDK version 1.42.4 onwards.
#if !defined(TAJO_SDK_VERSION_1_42_1)
      extendedCounterIds.insert(
          extendedCounterIds.end(),
          SaiQueueTraits::NonWatermarkEcnCounterIdsToRead.begin(),
          SaiQueueTraits::NonWatermarkEcnCounterIdsToRead.end());
#endif
    }
  }

  return extendedCounterIds;
}

const std::vector<sai_stat_id_t>&
SaiQueueManager::supportedWatermarkCounterIdsReadAndClear(int queueType) const {
  if (queueType == SAI_QUEUE_TYPE_FABRIC_TX) {
    static const std::vector<sai_stat_id_t> kFabricQueueWatermarksStats{
        SaiQueueTraits::WatermarkLevelCounterIdsToReadAndClear.begin(),
        SaiQueueTraits::WatermarkLevelCounterIdsToReadAndClear.end()};
    return kFabricQueueWatermarksStats;
  }
  static const std::vector<sai_stat_id_t> kWatermarkStats{
      SaiQueueTraits::WatermarkByteCounterIdsToReadAndClear.begin(),
      SaiQueueTraits::WatermarkByteCounterIdsToReadAndClear.end()};
  return kWatermarkStats;
}

void SaiQueueManager::updateStats(
    const std::vector<SaiQueueHandle*>& queueHandles,
    HwPortStats& hwPortStats,
    bool updateWatermarks) {
  hwPortStats.outCongestionDiscardPkts_() = 0;
  static std::vector<sai_stat_id_t> nonWatermarkStatsReadAndClear(
      SaiQueueTraits::NonWatermarkCounterIdsToReadAndClear.begin(),
      SaiQueueTraits::NonWatermarkCounterIdsToReadAndClear.end());
  for (auto queueHandle : queueHandles) {
    /*
     * The WRED_DROPPED_PACKETS counter is needed only for non-CPU
     * ports and on platform supporting ECN/WRED, which is taken
     * care of in the API supportedNonWatermarkCounterIdsRead().
     * Hence, not using queueHandle->queue->updateStats() directly.
     */
    auto queueType = SaiApiTable::getInstance()->queueApi().getAttribute(
        queueHandle->queue->adapterKey(), SaiQueueTraits::Attributes::Type{});

    queueHandle->queue->updateStats(
        supportedNonWatermarkCounterIdsRead(queueType, queueHandle),
        SAI_STATS_MODE_READ);
    queueHandle->queue->updateStats(
        nonWatermarkStatsReadAndClear, SAI_STATS_MODE_READ_AND_CLEAR);
    if (updateWatermarks) {
      queueHandle->queue->updateStats(
          supportedWatermarkCounterIdsReadAndClear(queueType),
          SAI_STATS_MODE_READ_AND_CLEAR);
    }
    const auto& counters = queueHandle->queue->getStats();
    auto queueId = SaiApiTable::getInstance()->queueApi().getAttribute(
        queueHandle->queue->adapterKey(), SaiQueueTraits::Attributes::Index{});
    fillHwQueueStats(queueId, counters, hwPortStats);
  }
}

void SaiQueueManager::clearStats(
    const std::vector<SaiQueueHandle*>& queueHandles) {
  for (auto& queueHandle : queueHandles) {
    auto& queue = queueHandle->queue;
    auto queueType = GET_ATTR(Queue, Type, queue->attributes());
    std::vector<sai_stat_id_t> toClear =
        supportedNonWatermarkCounterIdsRead(queueType, queueHandle);
    auto watermarkCounters =
        supportedWatermarkCounterIdsReadAndClear(queueType);
    toClear.insert(
        toClear.end(),
        std::make_move_iterator(watermarkCounters.begin()),
        std::make_move_iterator(watermarkCounters.end()));
    queue->clearStats(toClear);
  }
}

void SaiQueueManager::getStats(
    SaiQueueHandles& queueHandles,
    HwPortStats& hwPortStats) {
  for (auto& queueHandle : queueHandles) {
    const auto& counters = queueHandle.second->queue->getStats();
    fillHwQueueStats(queueHandle.first.first, counters, hwPortStats);
  }
}

void SaiQueueManager::updateStats(
    const std::vector<SaiQueueHandle*>& queueHandles,
    HwSysPortStats& hwSysPortStats,
    bool updateWatermarks) {
  static std::vector<sai_stat_id_t> nonWatermarkStatsReadAndClear(
      SaiQueueTraits::VoqNonWatermarkCounterIdsToReadAndClear.begin(),
      SaiQueueTraits::VoqNonWatermarkCounterIdsToReadAndClear.end());
  static std::vector<sai_stat_id_t> watermarkStatsReadAndClear(
      SaiQueueTraits::WatermarkByteCounterIdsToReadAndClear.begin(),
      SaiQueueTraits::WatermarkByteCounterIdsToReadAndClear.end());
  for (auto queueHandle : queueHandles) {
    auto queueType = GET_ATTR(Queue, Type, queueHandle->queue->attributes());
    queueHandle->queue->updateStats(
        supportedNonWatermarkCounterIdsRead(queueType, queueHandle),
        SAI_STATS_MODE_READ);
    queueHandle->queue->updateStats(
        nonWatermarkStatsReadAndClear, SAI_STATS_MODE_READ_AND_CLEAR);
    if (updateWatermarks) {
      queueHandle->queue->updateStats(
          watermarkStatsReadAndClear, SAI_STATS_MODE_READ_AND_CLEAR);
    }
    const auto& counters = queueHandle->queue->getStats();
    auto queueId = SaiApiTable::getInstance()->queueApi().getAttribute(
        queueHandle->queue->adapterKey(), SaiQueueTraits::Attributes::Index{});
    fillHwQueueStats(queueId, counters, hwSysPortStats);
  }
}
QueueConfig SaiQueueManager::getQueueSettings(
    const SaiQueueHandles& queueHandles) const {
  QueueConfig queueConfig;
  for (auto& queueHandle : queueHandles) {
    auto portQueue = std::make_shared<PortQueue>(queueHandle.first.first);
    portQueue->setStreamType(queueHandle.first.second);
    managerTable_->schedulerManager().fillSchedulerSettings(
        queueHandle.second->scheduler.get(), portQueue.get());
    queueConfig.push_back(std::move(portQueue));
  }
  return queueConfig;
}

} // namespace facebook::fboss
