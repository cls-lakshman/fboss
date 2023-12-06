// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "fboss/agent/test/AgentWrapperTest.h"

#include "fboss/agent/AgentCommandExecutor.h"
#include "fboss/lib/thrift_service_client/ThriftServiceClient.h"
#include "tupperware/agent/system/systemd/Service.h"

#include <folly/FileUtil.h>
#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "fboss/lib/CommonUtils.h"

#include "fboss/agent/AgentNetWhoAmI.h"
#include "fboss/lib/CommonFileUtils.h"

#include "fboss/agent/AgentConfig.h"

#include <thread>

DEFINE_int32(num_retries, 5, "number of retries for agent to start");
DEFINE_int32(wait_timeout, 15, "number of seconds to wait before retry");

namespace facebook::fboss {

void AgentWrapperTest::SetUp() {
  whoami_ = std::make_unique<AgentNetWhoAmI>();
  config_ = AgentConfig::fromFile("/etc/coop/agent/current");
  createDirectoryTree(util_.getWarmBootDir());
}

void AgentWrapperTest::TearDown() {
  stop();
}

void AgentWrapperTest::start() {
  AgentCommandExecutor executor;
  executor.startService("wedge_agent");
}

void AgentWrapperTest::stop() {
  AgentCommandExecutor executor;
  executor.stopService("wedge_agent");
}

void AgentWrapperTest::wait(bool started) {
  if (started) {
    waitForStart();
  } else {
    waitForStop();
  }
}

void AgentWrapperTest::waitForStart(const std::string& unit) {
  WITH_RETRIES_N_TIMED(
      FLAGS_num_retries, std::chrono::seconds(FLAGS_wait_timeout), {
        facebook::tupperware::systemd::Service service{unit};
        auto status = service.getStatus();
        EXPECT_EVENTUALLY_EQ(
            status.value().serviceState,
            facebook::tupperware::systemd::ProcessStatus::ServiceState::
                RUNNING);
      });

  auto client = utils::createWedgeAgentClient();
  apache::thrift::RpcOptions options;
  options.setTimeout(std::chrono::seconds(1));
  WITH_RETRIES_N_TIMED(
      FLAGS_num_retries, std::chrono::seconds(FLAGS_wait_timeout), {
        SwitchRunState runState = SwitchRunState::UNINITIALIZED;
        try {
          runState = client->sync_getSwitchRunState(options);
        } catch (const std::exception& ex) {
          XLOG(INFO) << "Waiting for wedge agent to start";
          continue;
        }
        EXPECT_EVENTUALLY_EQ(runState, SwitchRunState::CONFIGURED);
      });
}

void AgentWrapperTest::waitForStart() {
  auto multiSwitch = config_->getRunMode() == cfg::AgentRunMode::MULTI_SWITCH;
  if (multiSwitch) {
    // TODO: wait for hw_agent as well
    waitForStart("fboss_sw_agent.service");
  } else {
    waitForStart("wedge_agent.service");
  }
}

void AgentWrapperTest::waitForStop(const std::string& unit, bool crash) {
  WITH_RETRIES_N_TIMED(
      FLAGS_num_retries, std::chrono::seconds(FLAGS_wait_timeout), {
        facebook::tupperware::systemd::Service service{unit};
        auto status = service.waitForExit(
            std::chrono::microseconds(FLAGS_wait_timeout * 1000000));
        EXPECT_EVENTUALLY_EQ(
            status.value().serviceState,
            facebook::tupperware::systemd::ProcessStatus::ServiceState::EXITED);
        if (crash) {
          if (config_->getRunMode() == cfg::AgentRunMode::MULTI_SWITCH) {
            EXPECT_EVENTUALLY_EQ(status.value().exitCode, CLD_DUMPED);
          } else {
            EXPECT_EVENTUALLY_EQ(status.value().exitStatus, 255);
          }
        }
      });
}

void AgentWrapperTest::waitForStop(bool crash) {
  auto multiSwitch = (config_->getRunMode() == cfg::AgentRunMode::MULTI_SWITCH);
  if (multiSwitch) {
    waitForStop("fboss_sw_agent.service", crash);
    // TODO: wait for hw_agent as well
  } else {
    waitForStop("wedge_agent.service", crash);
  }
}

TEST_F(AgentWrapperTest, ColdBootStartAndStop) {
  auto drainTimeFile = util_.getRoutingProtocolColdBootDrainTimeFile();
  std::vector<char> data = {'0', '5'};
  if (!whoami_->isNotDrainable() && !whoami_->isFdsw()) {
    touchFile(drainTimeFile);
    folly::writeFile(data, drainTimeFile.c_str());
  }
  touchFile(util_.getColdBootOnceFile());
  touchFile(util_.getUndrainedFlag());
  start();
  waitForStart();
  if (!whoami_->isNotDrainable() && !whoami_->isFdsw()) {
    EXPECT_FALSE(checkFileExists(drainTimeFile));
  }
  stop();
  waitForStop();
  removeFile(util_.getRoutingProtocolColdBootDrainTimeFile());
  removeFile(util_.getUndrainedFlag());
}

TEST_F(AgentWrapperTest, StartAndStopAndStart) {
  touchFile(util_.getColdBootOnceFile());
  start();
  waitForStart();
  stop();
  waitForStop();
  start();
  waitForStart();
  stop();
  waitForStop();
}

TEST_F(AgentWrapperTest, StartAndCrash) {
  start();
  waitForStart();
  touchFile(util_.sleepSwSwitchOnSigTermFile());
  std::vector<char> sleepTime = {'3', '0', '0'};
  folly::writeFile(sleepTime, util_.sleepSwSwitchOnSigTermFile().c_str());
  auto maxPostSignalWaitTime = util_.getMaxPostSignalWaitTimeFile();
  touchFile(maxPostSignalWaitTime);
  std::vector<char> data = {'1'};
  folly::writeFile(data, maxPostSignalWaitTime.c_str());
  stop();
  waitForStop(true /* expect sigabrt to crash */);
}

} // namespace facebook::fboss

#ifdef IS_OSS
FOLLY_INIT_LOGGING_CONFIG("DBG2; default:async=true");
#else
FOLLY_INIT_LOGGING_CONFIG("fboss=DBG4; default:async=true");
#endif

int main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv, true);

  // Run the tests
  return RUN_ALL_TESTS();
}
