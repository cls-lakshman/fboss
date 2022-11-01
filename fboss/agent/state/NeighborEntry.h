/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/MacAddress.h>
#include <optional>
#include "fboss/agent/Utils.h"
#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/gen-cpp2/switch_state_types.h"
#include "fboss/agent/state/NodeBase.h"
#include "fboss/agent/state/PortDescriptor.h"
#include "fboss/agent/state/Thrifty.h"

namespace facebook::fboss {

using folly::MacAddress;

// TODO: remove this in favor of state::NeighborState in switch_state.thrift
enum class NeighborState { UNVERIFIED, PENDING, REACHABLE };

template <typename IPADDR>
struct NeighborEntryFields : public ThriftyFields<
                                 NeighborEntryFields<IPADDR>,
                                 state::NeighborEntryFields> {
  typedef IPADDR AddressType;

  NeighborEntryFields(
      AddressType ip,
      folly::MacAddress mac,
      PortDescriptor port,
      InterfaceID interfaceID,
      NeighborState state = NeighborState::REACHABLE,
      std::optional<cfg::AclLookupClass> classID = std::nullopt,
      std::optional<int64_t> encapIndex = std::nullopt,
      bool isLocal = true)
      : ip(ip),
        mac(mac),
        port(port),
        interfaceID(interfaceID),
        state(state),
        classID(classID),
        encapIndex(encapIndex),
        isLocal(isLocal) {}

  NeighborEntryFields(
      AddressType ip,
      InterfaceID interfaceID,
      NeighborState pending,
      std::optional<int64_t> encapIndex = std::nullopt,
      bool isLocal = true)
      : NeighborEntryFields(
            ip,
            MacAddress::BROADCAST,
            PortDescriptor(PortID(0)),
            interfaceID,
            pending,
            std::nullopt,
            encapIndex,
            isLocal) {
    // This constructor should only be used for PENDING entries
    CHECK(pending == NeighborState::PENDING);
  }

  template <typename Fn>
  void forEachChild(Fn /*fn*/) {}

  state::NeighborEntryFields toThrift() const override {
    state::NeighborEntryFields entryTh;
    entryTh.ipaddress() = ip.str();
    entryTh.mac() = mac.toString();
    entryTh.portId() = port.toThrift();
    entryTh.interfaceId() = static_cast<uint32_t>(interfaceID);
    entryTh.state() = static_cast<state::NeighborState>(state);
    if (classID.has_value()) {
      entryTh.classID() = classID.value();
    }
    if (encapIndex.has_value()) {
      entryTh.encapIndex() = encapIndex.value();
    }
    entryTh.isLocal() = isLocal;
    return entryTh;
  }

  static NeighborEntryFields fromThrift(
      state::NeighborEntryFields const& entryTh) {
    IPADDR ip(entryTh.get_ipaddress());
    folly::MacAddress mac(entryTh.get_mac());
    auto port = PortDescriptor::fromThrift(entryTh.get_portId());
    InterfaceID intf(entryTh.get_interfaceId());
    auto state = NeighborState(static_cast<int>(entryTh.get_state()));
    std::optional<int64_t> encapIndex;
    if (entryTh.encapIndex().has_value()) {
      encapIndex = *entryTh.encapIndex();
    }
    bool isLocal = *entryTh.isLocal();

    if (entryTh.classID().has_value() && !ip.isLinkLocal()) {
      return NeighborEntryFields(
          ip, mac, port, intf, state, *entryTh.classID(), encapIndex, isLocal);
    } else {
      return NeighborEntryFields(
          ip, mac, port, intf, state, std::nullopt, encapIndex, isLocal);
    }
  }

  /*
   * Serialize to folly::dynamic
   */
  folly::dynamic toFollyDynamicLegacy() const;

  /*
   * Deserialize from folly::dynamic
   */
  static NeighborEntryFields fromFollyDynamicLegacy(
      const folly::dynamic& entryJson);

  AddressType ip;
  folly::MacAddress mac;
  PortDescriptor port;
  InterfaceID interfaceID;
  NeighborState state;
  std::optional<cfg::AclLookupClass> classID{std::nullopt};
  std::optional<int64_t> encapIndex{std::nullopt};
  bool isLocal{true};
};

template <typename IPADDR, typename SUBCLASS>
class NeighborEntry : public thrift_cow::ThriftStructNode<
                          state::NeighborEntryFields,
                          thrift_cow::TypeIdentity<SUBCLASS>> {
 public:
  typedef IPADDR AddressType;

  NeighborEntry(
      AddressType ip,
      folly::MacAddress mac,
      PortDescriptor port,
      InterfaceID intfID,
      NeighborState state = NeighborState::REACHABLE,
      std::optional<cfg::AclLookupClass> classID = std::nullopt,
      std::optional<int64_t> encapIndex = std::nullopt,
      bool isLocal = true);

  NeighborEntry(
      AddressType ip,
      InterfaceID interfaceID,
      NeighborState pending,
      std::optional<int64_t> encapIndex = std::nullopt,
      bool isLocal = true);

  static std::shared_ptr<SUBCLASS> fromFollyDynamicLegacy(
      const folly::dynamic& json) {
    auto fields = NeighborEntryFields<IPADDR>::fromFollyDynamicLegacy(json);
    return std::make_shared<SUBCLASS>(fields.toThrift());
  }

  static std::shared_ptr<SUBCLASS> fromFollyDynamic(
      const folly::dynamic& json) {
    return fromFollyDynamicLegacy(json);
  }

  folly::dynamic toFollyDynamicLegacy() const {
    auto fields = NeighborEntryFields<IPADDR>::fromThrift(this->toThrift());
    return fields.toFollyDynamicLegacy();
  }

  folly::dynamic toFollyDynamic() const override {
    return toFollyDynamicLegacy();
  }

  void setIP(AddressType ip) {
    this->template set<switch_state_tags::ipaddress>(ip.str());
  }

  AddressType getIP() const {
    return AddressType(
        this->template get<switch_state_tags::ipaddress>()->cref());
  }

  folly::MacAddress getMac() const {
    return folly::MacAddress(
        this->template get<switch_state_tags::mac>()->cref());
  }
  void setMAC(folly::MacAddress mac) {
    this->template set<switch_state_tags::mac>(mac.toString());
  }

  void setPort(PortDescriptor port) {
    this->template set<switch_state_tags::portId>(port.toThrift());
  }
  PortDescriptor getPort() const {
    return PortDescriptor::fromThrift(
        this->template get<switch_state_tags::portId>()->toThrift());
  }

  NeighborState getState() const {
    return static_cast<NeighborState>(
        this->template get<switch_state_tags::state>()->cref());
  }

  bool nonZeroPort() const {
    return getPort() != PortID(0);
  }
  bool zeroPort() const {
    return !nonZeroPort();
  }

  InterfaceID getIntfID() const {
    return InterfaceID(
        this->template get<switch_state_tags::interfaceId>()->cref());
  }
  void setIntfID(InterfaceID id) {
    this->template set<switch_state_tags::interfaceId>(id);
  }

  NeighborState setState(NeighborState state) {
    this->template set<switch_state_tags::state>(
        static_cast<state::NeighborState>(state));
    return getState();
  }

  bool isPending() const {
    return this->getState() == NeighborState::PENDING;
  }

  void setPending() {
    this->setState(NeighborState::PENDING);
  }

  bool isReachable() const {
    return this->getState() == NeighborState::REACHABLE;
  }

  std::optional<cfg::AclLookupClass> getClassID() const {
    if (auto classID = this->template get<switch_state_tags::classID>()) {
      return classID->cref();
    }
    return std::nullopt;
  }

  void setClassID(std::optional<cfg::AclLookupClass> classID = std::nullopt) {
    if (classID) {
      this->template set<switch_state_tags::classID>(*classID);
    } else {
      this->template ref<switch_state_tags::classID>().reset();
    }
  }

  std::optional<int64_t> getEncapIndex() const {
    if (auto encapIndex = this->template get<switch_state_tags::encapIndex>()) {
      return encapIndex->cref();
    }
    return std::nullopt;
  }
  void setEncapIndex(std::optional<int64_t> encapIndex) {
    if (encapIndex) {
      this->template set<switch_state_tags::encapIndex>(*encapIndex);
    } else {
      this->template ref<switch_state_tags::encapIndex>().reset();
    }
  }

  bool getIsLocal() const {
    return this->template get<switch_state_tags::isLocal>()->cref();
  }
  void setIsLocal(bool isLocal) {
    this->template set<switch_state_tags::isLocal>(isLocal);
  }
  std::string str() const;

 private:
  typedef thrift_cow::ThriftStructNode<
      state::NeighborEntryFields,
      thrift_cow::TypeIdentity<SUBCLASS>>
      Parent;
  // Inherit the constructors required for clone()
  using Parent::Parent;
  friend class CloneAllocator;
};

} // namespace facebook::fboss
