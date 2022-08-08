/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/state/Interface.h"

#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <optional>
#include "fboss/agent/AddressUtil.h"
#include "fboss/agent/state/InterfaceMap.h"
#include "fboss/agent/state/NodeBase-defs.h"
#include "fboss/agent/state/SwitchState.h"
#include "folly/IPAddress.h"
#include "folly/MacAddress.h"

using folly::IPAddress;
using folly::MacAddress;
using folly::to;
using std::string;

namespace {
constexpr auto kInterfaceId = "interfaceId";
constexpr auto kRouterId = "routerId";
constexpr auto kVlanId = "vlanId";
constexpr auto kName = "name";
constexpr auto kMac = "mac";
constexpr auto kAddresses = "addresses";
constexpr auto kNdpConfig = "ndpConfig";
constexpr auto kMtu = "mtu";
constexpr auto kIsVirtual = "isVirtual";
constexpr auto kIsStateSyncDisabled = "isStateSyncDisabled";
} // namespace

namespace facebook::fboss {

InterfaceFields InterfaceFields::fromFollyDynamic(const folly::dynamic& json) {
  auto intfFields = InterfaceFields(
      InterfaceID(json[kInterfaceId].asInt()),
      RouterID(json[kRouterId].asInt()),
      VlanID(json[kVlanId].asInt()),
      json[kName].asString(),
      MacAddress(json[kMac].asString()),
      json[kMtu].asInt(),
      json.getDefault(kIsVirtual, false).asBool(),
      json.getDefault(kIsStateSyncDisabled, false).asBool());
  for (const auto& addr : json[kAddresses]) {
    auto cidr = IPAddress::createNetwork(
        addr.asString(),
        -1 /*use /32 for v4 and /128 for v6*/,
        false /*don't apply mask*/);
    intfFields.writableData().addresses()->emplace(
        cidr.first.str(), cidr.second);
  }
  intfFields.writableData().ndpConfig() =
      apache::thrift::SimpleJSONSerializer::deserialize<cfg::NdpConfig>(
          toJson(json[kNdpConfig]));
  return intfFields;
}

folly::dynamic InterfaceFields::toFollyDynamic() const {
  folly::dynamic intf = folly::dynamic::object;
  intf[kInterfaceId] = *data().interfaceId();
  intf[kRouterId] = *data().routerId();
  intf[kVlanId] = *data().vlanId();
  intf[kName] = *data().name();
  // fix
  intf[kMac] = to<string>(folly::MacAddress::fromNBO(*data().mac()));
  // fix?
  intf[kMtu] = to<string>(*data().mtu());
  // fix?
  intf[kIsVirtual] = to<string>(*data().isVirtual());
  // fix?
  intf[kIsStateSyncDisabled] = to<string>(*data().isStateSyncDisabled());
  std::vector<folly::dynamic> addresses;
  addresses.reserve(data().addresses()->size());
  for (auto const& [ipStr, mask] : *data().addresses()) {
    addresses.emplace_back(ipStr + "/" + folly::to<string>(mask));
  }
  intf[kAddresses] = folly::dynamic(addresses.begin(), addresses.end());
  string ndpCfgJson;
  apache::thrift::SimpleJSONSerializer::serialize(
      *data().ndpConfig(), &ndpCfgJson);
  intf[kNdpConfig] = folly::parseJson(ndpCfgJson);
  return intf;
}

std::optional<folly::CIDRNetwork> Interface::getAddressToReach(
    const folly::IPAddress& dest) const {
  for (const auto& [ipStr, mask] : *getFields()->data().addresses()) {
    auto cidr = folly::CIDRNetwork(ipStr, mask);
    if (dest.inSubnet(cidr.first, cidr.second)) {
      return cidr;
    }
  }
  return std::nullopt;
}

bool Interface::canReachAddress(const folly::IPAddress& dest) const {
  return getAddressToReach(dest).has_value();
}

bool Interface::isIpAttached(
    folly::IPAddress ip,
    InterfaceID intfID,
    const std::shared_ptr<SwitchState>& state) {
  const auto& allIntfs = state->getInterfaces();
  const auto intf = allIntfs->getInterfaceIf(intfID);
  if (!intf) {
    return false;
  }

  // Verify the address is reachable through this interface
  return intf->canReachAddress(ip);
}

template class NodeBaseT<Interface, InterfaceFields>;

} // namespace facebook::fboss
