/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/state/RouteTypes.h"
#include "fboss/agent/AddressUtil.h"

namespace {
constexpr auto kAddress = "address";
constexpr auto kMask = "mask";
constexpr auto kDrop = "Drop";
constexpr auto kToCpu = "ToCPU";
constexpr auto kNexthops = "Nexthops";
constexpr auto kLabel = "label";
} // namespace

namespace facebook::fboss {

std::string forwardActionStr(RouteForwardAction action) {
  switch (action) {
    case RouteForwardAction::DROP:
      return kDrop;
    case RouteForwardAction::TO_CPU:
      return kToCpu;
    case RouteForwardAction::NEXTHOPS:
      return kNexthops;
  }
  CHECK(0);
}

RouteForwardAction str2ForwardAction(const std::string& action) {
  if (action == kDrop) {
    return RouteForwardAction::DROP;
  } else if (action == kToCpu) {
    return RouteForwardAction::TO_CPU;
  } else {
    CHECK(action == kNexthops);
    return RouteForwardAction::NEXTHOPS;
  }
}

//
// RoutePrefix<> Class
//

template <typename AddrT>
bool RoutePrefix<AddrT>::operator<(const RoutePrefix& p2) const {
  if (mask() < p2.mask()) {
    return true;
  } else if (mask() > p2.mask()) {
    return false;
  }
  return network() < p2.network();
}

template <typename AddrT>
bool RoutePrefix<AddrT>::operator>(const RoutePrefix& p2) const {
  if (mask() > p2.mask()) {
    return true;
  } else if (mask() < p2.mask()) {
    return false;
  }
  return network() > p2.network();
}

template <typename AddrT>
folly::dynamic RoutePrefix<AddrT>::toFollyDynamicLegacy() const {
  folly::dynamic pfx = folly::dynamic::object;
  pfx[kAddress] = network().str();
  pfx[kMask] = mask();
  return pfx;
}

template <typename AddrT>
RoutePrefix<AddrT> RoutePrefix<AddrT>::fromFollyDynamicLegacy(
    const folly::dynamic& pfxJson) {
  return RoutePrefix(
      AddrT(pfxJson[kAddress].stringPiece()), pfxJson[kMask].asInt());
}

template <typename AddrT>
RoutePrefix<AddrT> RoutePrefix<AddrT>::fromString(std::string str) {
  std::vector<std::string> vec;

  folly::split("/", str, vec);
  CHECK_EQ(2, vec.size());
  auto prefix = RoutePrefix{AddrT(vec.at(0)), folly::to<uint8_t>(vec.at(1))};
  return prefix;
}

void toAppend(const RoutePrefixV4& prefix, std::string* result) {
  result->append(prefix.str());
}

void toAppend(const RoutePrefixV6& prefix, std::string* result) {
  result->append(prefix.str());
}

void toAppend(const RouteKeyMpls& route, std::string* result) {
  result->append(fmt::format("{}", route.label()));
}

void toAppend(const RouteForwardAction& action, std::string* result) {
  result->append(forwardActionStr(action));
}

std::ostream& operator<<(std::ostream& os, const RouteForwardAction& action) {
  os << forwardActionStr(action);
  return os;
}

folly::dynamic Label::toFollyDynamicLegacy() const {
  folly::dynamic pfx = folly::dynamic::object;
  pfx[kLabel] = static_cast<int32_t>(value());
  return pfx;
}

template <typename AddrT>
state::RoutePrefix RoutePrefix<AddrT>::toThrift() const {
  state::RoutePrefix thriftPrefix{};
  thriftPrefix.v6() = std::is_same_v<AddrT, folly::IPAddressV6>;
  thriftPrefix.prefix() = network::toBinaryAddress(folly::IPAddress(network_));
  thriftPrefix.mask() = mask_;
  return thriftPrefix;
}

template <typename AddrT>
RoutePrefix<AddrT> RoutePrefix<AddrT>::fromThrift(
    const state::RoutePrefix& thriftPrefix) {
  RoutePrefix<AddrT> prefix;
  prefix.mask_ = *thriftPrefix.mask();
  folly::IPAddress network = network::toIPAddress(*thriftPrefix.prefix());
  if constexpr (std::is_same_v<AddrT, folly::IPAddressV6>) {
    prefix.network_ = network.asV6();
  } else {
    prefix.network_ = network.asV4();
  }
  return prefix;
}

template <typename AddrT>
folly::dynamic RoutePrefix<AddrT>::migrateToThrifty(folly::dynamic const& dyn) {
  folly::dynamic newDyn = dyn;
  auto addr = ThriftyUtils::toThriftBinaryAddress(dyn[kAddress]);
  // byte is represented as signed char in thrift
  signed char mask = static_cast<signed char>(dyn[kMask].asInt());
  newDyn["prefix"] = ThriftyUtils::toFollyDynamic(addr);
  if constexpr (std::is_same_v<AddrT, folly::IPAddress>) {
    newDyn["v6"] = network::toIPAddress(addr).isV6();
  } else {
    newDyn["v6"] = std::is_same_v<AddrT, folly::IPAddressV6>;
  }
  newDyn["mask"] = mask;
  return newDyn;
}
template <typename AddrT>
void RoutePrefix<AddrT>::migrateFromThrifty(folly::dynamic& dyn) {
  auto ip = ThriftyUtils::toFollyIPAddress(dyn["prefix"]);
  dyn[kAddress] = ThriftyUtils::toFollyDynamic(ip);
  // convert signed char / byte to unsigned int
  uint8_t mask = static_cast<signed char>(dyn[kMask].asInt());
  dyn[kMask] = mask;
  dyn.erase("prefix");
  dyn.erase("v6");
}

Label Label::fromFollyDynamicLegacy(const folly::dynamic& prefixJson) {
  Label lbl;
  lbl.label_ = static_cast<int32_t>(prefixJson[kLabel].asInt());
  return lbl;
}

folly::dynamic Label::migrateToThrifty(folly::dynamic const& dyn) {
  folly::dynamic newDyn = folly::dynamic::object;
  newDyn["value"] = dyn[kLabel].asInt();
  return newDyn;
}

void Label::migrateFromThrifty(folly::dynamic& dyn) {
  auto label = dyn["value"].asInt();
  dyn.erase("value");
  dyn[kLabel] = label;
}

template struct RoutePrefix<folly::IPAddress>;
template struct RoutePrefix<folly::IPAddressV4>;
template struct RoutePrefix<folly::IPAddressV6>;

} // namespace facebook::fboss
