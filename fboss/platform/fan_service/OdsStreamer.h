// Copyright 2021- Facebook. All rights reserved.

#pragma once
// OdsStreamer is a part of Fan Service
// Role : To stream the sensor data to ODS.

// Folly library header file for conversion and log
#include <folly/Conv.h>
#include <folly/logging/xlog.h>
// FB service routines for streaming ODS data
#include <fb303/ThreadCachedServiceData.h>
#include "common/fbwhoami/FbWhoAmI.h"
#include "common/network/NetworkUtil.h"
#include "common/strings/UUID.h"
#include "common/time/Time.h"
#include "common/time/TimeUtil.h"
#include "maestro/if/OdsRouter/gen-cpp2/OdsRouter.h"
#include "monitoring/common/OdsCategoryId.h"
#include "scribe/client/ScribeClient.h"
#include "servicerouter/client/cpp2/ServiceRouter.h"
// Headerfile for SensorData class
#include "SensorData.h"

namespace facebook::fboss::platform {
class OdsStreamer {
 public:
  // Constructor / Destructor
  OdsStreamer(const std::string& odsTier) : odsTier_(odsTier) {}
  // Main entry to initiate the ODS data send
  int postData(folly::EventBase* evb, const SensorData& sensorData) const;

 private:
  const std::string odsTier_;
  // Internal helper functions
  int publishToOds(
      folly::EventBase* evb,
      std::vector<facebook::maestro::ODSAppValue> values,
      std::string odsTier) const;
  facebook::maestro::ODSAppValue
  getOdsAppValue(std::string key, int64_t value, uint64_t timeStampSec) const;
};
} // namespace facebook::fboss::platform
