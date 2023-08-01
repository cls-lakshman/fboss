// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include "fboss/agent/AgentInitializer.h"

#include <folly/io/async/EventBase.h>

namespace facebook::fboss {

class SwAgentInitializer : public AgentInitializer {
 public:
  SwAgentInitializer() {}
  ~SwAgentInitializer() override {}
  int initAgent() override;
  void stopAgent(bool setupWarmboot) override;

 private:
  folly::EventBase dummyEvb_;
};

} // namespace facebook::fboss
