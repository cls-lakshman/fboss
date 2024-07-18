// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "fboss/fsdb/oper/Subscription.h"
#include "fboss/fsdb/oper/SubscriptionPathStore.h"

namespace facebook::fboss::fsdb {

class SubscriptionStore {
 public:
  virtual ~SubscriptionStore();

  void pruneCancelledSubscriptions();

  virtual void registerSubscription(std::unique_ptr<Subscription> subscription);

  void registerExtendedSubscription(
      std::shared_ptr<ExtendedSubscription> subscription);

  void registerPendingSubscriptions(
      std::vector<std::unique_ptr<Subscription>>&& subscriptions,
      std::vector<std::shared_ptr<ExtendedSubscription>>&&
          extendedSubscriptions);

  void unregisterSubscription(const std::string& name);

  void unregisterExtendedSubscription(const std::string& name);

  void processAddedPath(
      std::vector<std::string>::const_iterator begin,
      std::vector<std::string>::const_iterator end);

  void serveHeartbeat();

  void closeNoPublisherActiveSubscriptions(
      const SubscriptionMetadataServer& metadataServer,
      FsdbErrorCode disconnectReason);

  void flush(const SubscriptionMetadataServer& metadataServer);

  const auto& subscriptions() const {
    return subscriptions_;
  }
  const auto& extendedSubscriptions() const {
    return extendedSubscriptions_;
  }
  const auto& initialSyncNeeded() const {
    return initialSyncNeeded_;
  }
  const auto& initialSyncNeededExtended() const {
    return initialSyncNeededExtended_;
  }
  const auto& lookup() const {
    return lookup_;
  }
  auto& subscriptions() {
    return subscriptions_;
  }
  auto& extendedSubscriptions() {
    return extendedSubscriptions_;
  }
  auto& initialSyncNeeded() {
    return initialSyncNeeded_;
  }
  auto& initialSyncNeededExtended() {
    return initialSyncNeededExtended_;
  }
  auto& lookup() {
    return lookup_;
  }

 private:
  std::vector<std::string> markExtendedSubscriptionsThatNeedPruning();

  void pruneSimpleSubscriptions();

  void pruneExtendedSubscriptions(const std::vector<std::string>& toDelete);

  void registerSubscription(
      std::string name,
      std::unique_ptr<Subscription> subscription);

  void registerExtendedSubscription(
      std::string name,
      std::shared_ptr<ExtendedSubscription> subscription);

  // owned subscriptions, keyed on name they were registered with
  std::unordered_map<std::string, std::unique_ptr<Subscription>> subscriptions_;
  std::unordered_map<std::string, std::shared_ptr<ExtendedSubscription>>
      extendedSubscriptions_;
  SubscriptionPathStore initialSyncNeeded_;
  std::unordered_set<std::shared_ptr<ExtendedSubscription>>
      initialSyncNeededExtended_;

  // lookup for the subscriptions, keyed on path
  SubscriptionPathStore lookup_;
};

} // namespace facebook::fboss::fsdb
