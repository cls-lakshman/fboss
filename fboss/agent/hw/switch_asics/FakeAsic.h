// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include "fboss/agent/hw/switch_asics/HwAsic.h"

#include <set>

namespace facebook::fboss {

class FakeAsic : public HwAsic {
 public:
  bool isSupported(Feature feature) const override {
    switch (feature) {
      case Feature::HSDK:
      case Feature::OBJECT_KEY_CACHE:
      case Feature::RESOURCE_USAGE_STATS:
      case Feature::PKTIO:
      case Feature::WEIGHTED_NEXTHOPGROUP_MEMBER:
      case Feature::INGRESS_FIELD_PROCESSOR_FLEX_COUNTER:
      case Feature::OBM_COUNTERS:
      case Feature::PTP_TC:
      case Feature::PTP_TC_PCS:
      case Feature::EGRESS_QUEUE_FLEX_COUNTER:
        return false;

      default:
        return true;
    }
  }
  AsicType getAsicType() const override {
    return AsicType::ASIC_TYPE_FAKE;
  }
  std::string getVendor() const override {
    return "fake";
  }
  cfg::PortSpeed getMaxPortSpeed() const override {
    return cfg::PortSpeed::HUNDREDG;
  }
  std::set<cfg::StreamType> getQueueStreamTypes(bool cpu) const override {
    if (cpu) {
      return {cfg::StreamType::MULTICAST};
    } else {
      return {cfg::StreamType::UNICAST};
    }
  }
  int getDefaultNumPortQueues(cfg::StreamType streamType, bool /*cpu*/)
      const override {
    return streamType == cfg::StreamType::UNICAST ? 8 : 10;
  }
  uint32_t getMaxLabelStackDepth() const override {
    // Copying TH3's max label stack depth
    return 9;
  }
  uint64_t getMMUSizeBytes() const override {
    // Fake MMU size
    return 64 * 1024 * 1024;
  }
  uint32_t getMaxMirrors() const override {
    return 4;
  }
  uint64_t getDefaultReservedBytes(cfg::StreamType /*streamType*/, bool cpu)
      const override {
    // Mimicking TH
    return cpu ? 1664 : 0;
  }
  cfg::MMUScalingFactor getDefaultScalingFactor(
      cfg::StreamType /*streamType*/,
      bool /*cpu*/) const override {
    // Mimicking TH
    return cfg::MMUScalingFactor::TWO;
  }
  int getMaxNumLogicalPorts() const override {
    return 134;
  }
};

} // namespace facebook::fboss
