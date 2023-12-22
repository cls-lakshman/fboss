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
#include <memory>

#include <gflags/gflags.h>
#include <string>

#include "fboss/agent/CommonInit.h"
#include "fboss/agent/FbossInit.h"
#include "fboss/agent/HwAgent.h"

namespace facebook::fboss {

class SplitHwAgentSignalHandler : public SignalHandler {
 public:
  SplitHwAgentSignalHandler(
      folly::EventBase* eventBase,
      SignalHandler::StopServices stopServices,
      std::unique_ptr<HwAgent> hwAgent)
      : SignalHandler(eventBase, std::move(stopServices)),
        hwAgent_(std::move(hwAgent)) {}

  void signalReceived(int /*signum*/) noexcept override;

 private:
  std::unique_ptr<HwAgent> hwAgent_;
};

void setSDKVersionInfo();

std::string getSDKVersion();

int hwAgentMain(
    int argc,
    char** argv,
    uint32_t hwFeaturesDesired,
    PlatformInitFn initPlatformFn);

} // namespace facebook::fboss
