/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/hw/bcm/tests/BcmTest.h"

#include "fboss/agent/ApplyThriftConfig.h"
#include "fboss/agent/packet/Ethertype.h"
#include "fboss/agent/state/AggregatePort.h"
#include "fboss/agent/state/Port.h"
#include "fboss/agent/state/PortMap.h"
#include "fboss/agent/state/StateDelta.h"
#include "fboss/agent/state/SwitchState.h"
#include "fboss/agent/test/TrunkUtils.h"

#include "fboss/agent/gen-cpp2/switch_config_types.h"
#include "fboss/agent/hw/bcm/BcmError.h"
#include "fboss/agent/hw/bcm/BcmTrunkTable.h"
#include "fboss/agent/hw/bcm/tests/BcmLinkStateDependentTests.h"
#include "fboss/agent/platforms/tests/utils/BcmTestPlatform.h"

#include "fboss/agent/hw/test/HwTestTrunkUtils.h"

#include "fboss/agent/hw/test/ConfigFactory.h"

#include <folly/IPAddress.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <numeric>
#include <vector>

extern "C" {
#include <bcm/l3.h>
#include <bcm/pkt.h>
#include <bcm/port.h>
#include <bcm/trunk.h>
}

using facebook::fboss::bcmCheckError;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::IPAddressV6;

namespace facebook::fboss {

using utility::addAggPort;
using utility::enableTrunkPorts;

class BcmTrunkTest : public BcmLinkStateDependentTests {
 protected:
  cfg::SwitchConfig initialConfig() const override {
    return utility::oneL3IntfTwoPortConfig(
        getHwSwitch(),
        masterLogicalPortIds()[0],
        masterLogicalPortIds()[1],
        cfg::PortLoopbackMode::MAC);
  }

  void applyConfigAndEnableTrunks(const cfg::SwitchConfig& switchCfg) {
    auto state = applyNewConfig(switchCfg);
    applyNewState(enableTrunkPorts(state));
  }
};

TEST_F(BcmTrunkTest, TrunkCreateHighLowKeyIds) {
  auto setup = [=]() {
    auto cfg = initialConfig();
    addAggPort(
        std::numeric_limits<AggregatePortID>::max(),
        {masterLogicalPortIds()[0]},
        &cfg);
    addAggPort(1, {masterLogicalPortIds()[1]}, &cfg);
    applyConfigAndEnableTrunks(cfg);
  };
  auto verify = [=]() {
    utility::verifyAggregatePortCount(getHwSwitchEnsemble(), 2);
    utility::verifyAggregatePort(
        getHwSwitchEnsemble(), AggregatePortID(1)

    );
    utility::verifyAggregatePort(
        getHwSwitchEnsemble(),
        AggregatePortID(std::numeric_limits<AggregatePortID>::max())

    );
    auto aggIDs = {
        AggregatePortID(1),
        AggregatePortID(std::numeric_limits<AggregatePortID>::max())};
    for (auto aggId : aggIDs) {
      utility::verifyAggregatePortMemberCount(
          getHwSwitchEnsemble(), aggId, 1, 1);
    }
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(BcmTrunkTest, TrunkCheckIngressPktAggPort) {
  auto setup = [=]() {
    auto cfg = initialConfig();
    addAggPort(
        std::numeric_limits<AggregatePortID>::max(),
        {masterLogicalPortIds()[0]},
        &cfg);
    applyConfigAndEnableTrunks(cfg);
  };
  auto verify = [=]() {
    utility::verifyPktFromAggregatePort(
        getHwSwitchEnsemble(),
        AggregatePortID(std::numeric_limits<AggregatePortID>::max()));
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(BcmTrunkTest, TrunkMemberPortDownMinLinksViolated) {
  auto aggId = AggregatePortID(std::numeric_limits<AggregatePortID>::max());

  auto setup = [=]() {
    auto cfg = initialConfig();
    addAggPort(
        aggId, {masterLogicalPortIds()[0], masterLogicalPortIds()[1]}, &cfg);
    applyConfigAndEnableTrunks(cfg);

    bringDownPort(PortID(masterLogicalPortIds()[0]));
    // Member port count should drop to 1 now.
  };
  auto verify = [=]() {
    utility::verifyAggregatePortCount(getHwSwitchEnsemble(), 1);
    utility::verifyAggregatePort(getHwSwitchEnsemble(), aggId);
    utility::verifyAggregatePortMemberCount(getHwSwitchEnsemble(), aggId, 2, 1);
  };
  verifyAcrossWarmBoots(setup, verify);
}

TEST_F(BcmTrunkTest, findTrunkApiChecks) {
  static const AggregatePortID aggID{42};

  auto setup = [=]() {
    auto cfg = initialConfig();
    addAggPort(
        aggID, {masterLogicalPortIds()[0], masterLogicalPortIds()[1]}, &cfg);
    applyConfigAndEnableTrunks(cfg);
  };
  auto verify = [=]() {
    auto trunkTable = getHwSwitch()->getTrunkTable();

    EXPECT_EQ(
        BcmTrunk::findTrunk(
            getUnit(), static_cast<bcm_module_t>(0), masterLogicalPortIds()[0])
            .value(),
        trunkTable->getBcmTrunkId(aggID));
    EXPECT_EQ(
        BcmTrunk::findTrunk(
            getUnit(), static_cast<bcm_module_t>(0), masterLogicalPortIds()[1])
            .value(),
        trunkTable->getBcmTrunkId(aggID));
    EXPECT_EQ(
        BcmTrunk::findTrunk(
            getUnit(), static_cast<bcm_module_t>(0), masterLogicalPortIds()[2]),
        std::nullopt);
  };
  verifyAcrossWarmBoots(setup, verify);
}

} // namespace facebook::fboss
