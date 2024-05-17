// Copyright 2021-present Facebook. All Rights Reserved.

// Handler class handles Fan Service thrift calls.
#pragma once
#include <folly/futures/Future.h>

#include "fboss/platform/fan_service/FanServiceImpl.h"
#include "fboss/platform/fan_service/if/gen-cpp2/FanService.h"

namespace facebook::fboss::platform::fan_service {
class FanServiceHandler : public apache::thrift::ServiceHandler<FanService> {
 public:
  explicit FanServiceHandler(std::unique_ptr<FanServiceImpl> fanServiceImpl);
  // Make compiler happy
  ~FanServiceHandler() override = default;
  // Simple functions are just implemented here.
  FanServiceImpl* getFanServiceImpl() const {
    return fanServiceImpl_.get();
  }
  void getFanStatuses(FanStatusesResponse&) override;

  void setHold(std::unique_ptr<HoldRequest> req) override;
  void getHold(HoldStatus& status) override;

 private:
  // Internal pointer for FanServiceImpl.
  std::unique_ptr<FanServiceImpl> fanServiceImpl_{nullptr};
  std::optional<int> holdPwm_;
};
} // namespace facebook::fboss::platform::fan_service
