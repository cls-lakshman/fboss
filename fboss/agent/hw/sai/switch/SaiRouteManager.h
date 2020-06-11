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

#include "fboss/agent/hw/sai/api/NextHopGroupApi.h"
#include "fboss/agent/hw/sai/api/RouteApi.h"
#include "fboss/agent/hw/sai/store/SaiObject.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/types.h"

#include "folly/container/F14Map.h"

#include "fboss/agent/hw/sai/store/SaiObjectEventSubscriber.h"

#include <memory>
#include <mutex>

namespace facebook::fboss {

class SaiManagerTable;
class SaiPlatform;
class SaiNextHopGroupHandle;

using SaiRoute = SaiObject<SaiRouteTraits>;

template <typename T>
class SaiNeighborSubscriberForNextHop;

template <typename NextHopTraitsT>
class SaiRouteNextHopHandle
    : public detail::SaiObjectEventSubscriber<NextHopTraitsT> {
 public:
  using PublisherObject = std::shared_ptr<const SaiObject<NextHopTraitsT>>;
  SaiRouteNextHopHandle(
      SaiManagerTable* managerTable,
      SaiRouteTraits::AdapterHostKey routeKey,
      std::shared_ptr<SaiNeighborSubscriberForNextHop<NextHopTraitsT>>
          subscriber);
  void afterCreate(PublisherObject nexthop) override;
  void beforeRemove() override;

 private:
  SaiManagerTable* managerTable_;
  typename SaiRouteTraits::AdapterHostKey routeKey_;
  std::shared_ptr<SaiNeighborSubscriberForNextHop<NextHopTraitsT>> subscriber_;
};

using SaiRouteIpNextHopHandle = SaiRouteNextHopHandle<SaiIpNextHopTraits>;
using SaiRouteMplsNextHopHandle = SaiRouteNextHopHandle<SaiMplsNextHopTraits>;

struct SaiRouteHandle {
  std::shared_ptr<SaiNextHopGroupHandle> nextHopGroupHandle;
  std::shared_ptr<SaiRoute> route;
};

class SaiRouteManager {
 public:
  SaiRouteManager(SaiManagerTable* managerTable, const SaiPlatform* platform);
  // Helper function to create a SAI RouteEntry from an FBOSS SwitchState
  // Route (e.g., Route<IPAddressV6>)
  template <typename AddrT>
  SaiRouteTraits::RouteEntry routeEntryFromSwRoute(
      RouterID routerId,
      const std::shared_ptr<Route<AddrT>>& swEntry) const;

  template <typename AddrT>
  void changeRoute(
      const std::shared_ptr<Route<AddrT>>& oldSwRoute,
      const std::shared_ptr<Route<AddrT>>& newSwRoute,
      RouterID routerId);

  std::vector<std::shared_ptr<SaiRoute>> makeInterfaceToMeRoutes(
      const std::shared_ptr<Interface>& swInterface);

  template <typename AddrT>
  void addRoute(
      const std::shared_ptr<Route<AddrT>>& swRoute,
      RouterID routerId);

  template <typename AddrT>
  void removeRoute(
      const std::shared_ptr<Route<AddrT>>& swRoute,
      RouterID routerId);

  SaiRouteHandle* getRouteHandle(const SaiRouteTraits::RouteEntry& entry);
  const SaiRouteHandle* getRouteHandle(
      const SaiRouteTraits::RouteEntry& entry) const;

  void clear();

 private:
  SaiRouteHandle* getRouteHandleImpl(
      const SaiRouteTraits::RouteEntry& entry) const;
  template <typename AddrT>
  void addOrUpdateRoute(
      SaiRouteHandle* routeHandle,
      RouterID routerId,
      const std::shared_ptr<Route<AddrT>>& oldRoute,
      const std::shared_ptr<Route<AddrT>>& newRoute);

  template <typename AddrT>
  bool validRoute(const std::shared_ptr<Route<AddrT>>& swRoute);

  SaiManagerTable* managerTable_;
  const SaiPlatform* platform_;
  folly::F14FastMap<SaiRouteTraits::RouteEntry, std::unique_ptr<SaiRouteHandle>>
      handles_;
};

} // namespace facebook::fboss
