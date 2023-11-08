// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>

#include <folly/experimental/TestUtil.h>
#include <folly/logging/xlog.h>
#include "common/time/Time.h"
#include "fboss/lib/CommonUtils.h"
#include "fboss/qsfp_service/QsfpConfig.h"
#include "fboss/qsfp_service/test/hw_test/HwPortUtils.h"
#include "fboss/qsfp_service/test/hw_test/HwTest.h"

using namespace ::testing;

namespace facebook::fboss {

class OpticsFwUpgradeTest : public HwTest {
 public:
  OpticsFwUpgradeTest(bool setupOverrideTcvrToPortAndProfile = true)
      : HwTest(setupOverrideTcvrToPortAndProfile) {}

  void SetUp() override {
    // Enable the firmware_upgrade_supported flag for these tests. Without this,
    // firmware upgrade functionality will be skipped
    gflags::SetCommandLineOptionWithMode(
        "firmware_upgrade_supported", "1", gflags::SET_FLAGS_DEFAULT);
    // Set max concurrent evb fw upgrade to 8 for cold boot init before the test
    // starts This ensures that all the transceivers have a chance to upgrade to
    // the latest version of the firmware from the config before the actual test
    // starts later
    gflags::SetCommandLineOptionWithMode(
        "max_concurrent_evb_fw_upgrade", "8", gflags::SET_FLAGS_DEFAULT);
    HwTest::SetUp();
    // Revert the max_concurrent_evb_fw_upgrade back to 1 which is the default
    gflags::SetCommandLineOptionWithMode(
        "max_concurrent_evb_fw_upgrade", "1", gflags::SET_FLAGS_DEFAULT);
  }

  void printDebugInfo(
      TransceiverID tcvrID,
      bool upgradeExpected,
      long upgradeSinceMs,
      const TcvrStats& tcvrStats) const {
    XLOG(INFO) << "Transceiver " << tcvrID
               << " :upgradeExpected = " << upgradeExpected
               << " :lastFwUpgradeStartTime = "
               << *tcvrStats.lastFwUpgradeStartTime()
               << ", lastFwUpgradeEndTime = "
               << *tcvrStats.lastFwUpgradeEndTime()
               << " :upgradeSince = " << upgradeSinceMs;
  }

  // Helper function that verifies if an upgrade was done or not
  // Returns true if upgrade was done and was expected to be done, false
  // otherwise
  // Helper function that verifies if an upgrade was done or not
  // Returns true if upgrade was done and was expected to be done, false
  // otherwise
  bool verifyUpgrade(
      bool upgradeExpected,
      long upgradeSinceTsSec,
      std::vector<int32_t> tcvrs) {
    if (tcvrs.empty()) {
      tcvrs = utility::legacyTransceiverIds(
          utility::getCabledPortTranceivers(getHwQsfpEnsemble()));
    }
    bool result = true;
    for (auto tcvrID : tcvrs) {
      auto tcvrInfo =
          getHwQsfpEnsemble()->getWedgeManager()->getTransceiverInfo(
              TransceiverID(tcvrID));
      auto& tcvrStats = *tcvrInfo.tcvrStats();
      if (upgradeExpected &&
          (*tcvrStats.lastFwUpgradeStartTime() <= upgradeSinceTsSec ||
           *tcvrStats.lastFwUpgradeEndTime() <=
               *tcvrStats.lastFwUpgradeStartTime())) {
        printDebugInfo(
            TransceiverID(tcvrID),
            upgradeExpected,
            upgradeSinceTsSec,
            tcvrStats);
        result = false;
      } else if (
          !upgradeExpected &&
          (*tcvrStats.lastFwUpgradeStartTime() > upgradeSinceTsSec ||
           *tcvrStats.lastFwUpgradeEndTime() > upgradeSinceTsSec)) {
        printDebugInfo(
            TransceiverID(tcvrID),
            upgradeExpected,
            upgradeSinceTsSec,
            tcvrStats);
        result = false;
      }
    }
    return result;
  }

  // Helper function that sets the port status of all transceivers to the given
  // status. It does that by setting setOverrideAgentPortStatusForTesting to
  // true and then refreshing the state machines so that the state can be
  // transitioned to ACTIVE or INACTIVE states.
  void setPortStatus(bool status) {
    auto wedgeMgr = getHwQsfpEnsemble()->getWedgeManager();
    wedgeMgr->setOverrideAgentPortStatusForTesting(status, true /* enabled */);
    wedgeMgr->refreshStateMachines();

    auto cabledTransceivers = utility::legacyTransceiverIds(
        utility::getCabledPortTranceivers(getHwQsfpEnsemble()));
    for (auto id : cabledTransceivers) {
      auto curState = wedgeMgr->getCurrentState(TransceiverID(id));
      if (status) {
        EXPECT_EQ(curState, TransceiverStateMachineState::ACTIVE)
            << "Transceiver:" << id
            << " Actual: " << apache::thrift::util::enumNameSafe(curState)
            << ", Expected: ACTIVE";
      } else {
        // When forcing link down in tests, we also end up triggering
        // remediation since initial_remediate_interval is set to 0 in HwTest
        // SetUp. Since not every module can be remediated, the expected states
        // when forcing link down are either INACTIVE or XPHY_PORTS_PROGRAMMED
        // In addition, when firmware upgrade happens, the state machine
        // transitions from INACTIVE to UPGRADING to NOT_PRESENT and then
        // eventually to IPHY_PORTS_PROGRAMMED state. Therefore expect that too
        // when forcing link down
        EXPECT_TRUE(
            curState == TransceiverStateMachineState::IPHY_PORTS_PROGRAMMED ||
            curState == TransceiverStateMachineState::XPHY_PORTS_PROGRAMMED ||
            curState == TransceiverStateMachineState::INACTIVE)
            << "Transceiver:" << id
            << " Actual: " << apache::thrift::util::enumNameSafe(curState)
            << ", Expected: IPHY_PORTS_PROGRAMMED or XPHY_PORTS_PROGRAMMED or INACTIVE";
      }
    }
  }
};

TEST_F(OpticsFwUpgradeTest, noUpgradeForSameVersion) {
  // In this test, firmware versions in qsfp config is not changed. Hence, the
  // firmware upgrade shouldn't be triggered under any circumstances.
  // 1. Coldboot init might still trigger firmware upgrade depending on what the
  // firmware version was before the test started and what the firmware version
  // is in the qsfp config the test is run with
  // 2. Warmboot init, verify there were no upgrades done during warm boot.
  // 3. Force links to go down and verify there are no upgrades done

  long initDoneTimestampSec = facebook::WallClockUtil::NowInSecFast();
  auto verify = [&]() {
    if (didWarmBoot()) {
      CHECK(verifyUpgrade(
          false /* upgradeExpected */,
          0 /* upgradeSinceTsSec */,
          {} /* tcvrs */))
          << "No upgrades expected during warm boot";
    }
    // Force link up
    setPortStatus(true);
    CHECK(verifyUpgrade(
        false /* upgradeExpected */,
        initDoneTimestampSec /* upgradeSinceTsSec */,
        {} /* tcvrs */))
        << "No upgrades expected on port up";
    // Force link down
    setPortStatus(false);
    CHECK(verifyUpgrade(
        false /* upgradeExpected */,
        initDoneTimestampSec /* upgradeSinceTsSec */,
        {} /* tcvrs */))
        << "No upgrades expected on port down";
  };
  verifyAcrossWarmBoots([]() {}, verify);
}

TEST_F(OpticsFwUpgradeTest, upgradeOnLinkDown) {
  /*
   * This test verifies that a link down event successfully triggers and
   * completes the firmware upgrade
   * ------------------------------------------------------------------------
   * Coldboot
   * - Create a new QSFP config with qsfpConfig.transceiverFirmwareVersions =
   * qsfpConfig.qsfpTestConfig.firmwareForUpgradeTest
   * - Load the new config. This ensures that the current operational firmware
   * is different than the one in the config, making this optic eligible for
   * firmware upgrade
   * - Set link status to up, verify upgrade DOES NOT happen
   * - Set link status to down, verify upgrade happens
   * ------------------------------------------------------------------------
   * Warmboot with the original config (i.e.
   * qsfpConfig.transceiverFirmwareVersions again changes)
   * - Verify upgrade did not happen during warm boot
   * - Set link status to up, verify upgrade DOES NOT happen
   * - Set link status to down, verify upgrade happens
   */

  long initDoneTimestampSec = facebook::WallClockUtil::NowInSecFast();
  std::vector<int32_t> tcvrsToTest;
  auto allTransceivers = utility::legacyTransceiverIds(
      utility::getCabledPortTranceivers(getHwQsfpEnsemble()));
  auto& qsfpTestCfg = *getHwQsfpEnsemble()
                           ->getWedgeManager()
                           ->getQsfpConfig()
                           ->thrift.qsfpTestConfig();

  // Gather all transceivers that we need to include as part of this test
  // The criteria is that the qsfp test config defines a firmware version for
  // the transceiver part number
  for (auto tcvrID : allTransceivers) {
    auto tcvrInfo = getHwQsfpEnsemble()->getWedgeManager()->getTransceiverInfo(
        TransceiverID(tcvrID));
    auto& vendorPN = *tcvrInfo.tcvrState()->vendor().ensure().partNumber();
    if (qsfpTestCfg.firmwareForUpgradeTest()->versionsMap()->find(vendorPN) !=
        qsfpTestCfg.firmwareForUpgradeTest()->versionsMap()->end()) {
      tcvrsToTest.push_back(tcvrID);
    }
  }

  // Setup function is only called for cold boot iteration of the test
  // The setup below will create a new qsfp config with the different firmware
  // version and then load the new config
  auto setup = [&]() {
    // During cold boot setup, update the firmware versions in the config
    auto qsfpCfg =
        getHwQsfpEnsemble()->getWedgeManager()->getQsfpConfig()->thrift;
    qsfpCfg.transceiverFirmwareVersions() =
        *qsfpCfg.qsfpTestConfig()->firmwareForUpgradeTest();
    std::string newCfgStr =
        apache::thrift::SimpleJSONSerializer::serialize<std::string>(qsfpCfg);
    auto newQsfpCfg = QsfpConfig::fromRawConfig(newCfgStr);
    folly::test::TemporaryDirectory tmpDir = folly::test::TemporaryDirectory();
    std::string newCfgPath =
        tmpDir.path().string() + "/optics_upgrade_test_config";
    newQsfpCfg->dumpConfig(newCfgPath);
    FLAGS_qsfp_config = newCfgPath;
    getHwQsfpEnsemble()->getWedgeManager()->loadConfig();
  };

  // Verify function is called for both cold boot and warm boot iterations of
  // the test
  auto verify = [&, tcvrsToTest]() {
    if (didWarmBoot()) {
      CHECK(verifyUpgrade(
          false /* upgradeExpected */,
          0 /* upgradeSinceTsSec */,
          {} /* tcvrs */))
          << "No upgrades expected during warm boot";
      // Cold boot might do an upgrade depending on what the firmware
      // was left on the transceiver at the end of the last test
    } else {
      CHECK(verifyUpgrade(
          false /* upgradeExpected */,
          initDoneTimestampSec /* upgradeSinceTsSec */,
          {} /* tcvrs */))
          << "No upgrades expected during cold boot";
      // Cold boot might do an upgrade depending on what the firmware
      // was left on the transceiver at the end of the last test
    }
    // Force link up
    setPortStatus(true);
    CHECK(verifyUpgrade(
        false /* upgradeExpected */,
        initDoneTimestampSec /* upgradeSinceTsSec */,
        {} /* tcvrs */))
        << "No upgrades expected on port up";
    // Force link down
    setPortStatus(false);
    WITH_RETRIES_N_TIMED(
        8 /* retries */, std::chrono::milliseconds(1000) /* msBetweenRetry */, {
          getHwQsfpEnsemble()->getWedgeManager()->refreshStateMachines();
          EXPECT_EVENTUALLY_TRUE(verifyUpgrade(
              true /* upgradeExpected */,
              initDoneTimestampSec /* upgradeSinceTsSec */,
              tcvrsToTest /* tcvrs */));
        });
  };

  verifyAcrossWarmBoots(setup, verify);
}
} // namespace facebook::fboss
