/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include "fboss/agent/hw/test/dataplane_tests/HwProdInvariantHelper.h"

#include <folly/logging/xlog.h>
#include "fboss/agent/hw/test/ConfigFactory.h"
#include "fboss/agent/hw/test/HwTestCoppUtils.h"
#include "fboss/agent/hw/test/HwTestPacketUtils.h"
#include "fboss/agent/hw/test/LoadBalancerUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwEcmpDataPlaneTestUtil.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestOlympicUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestQosUtils.h"
#include "fboss/agent/hw/test/dataplane_tests/HwTestQueuePerHostUtils.h"
#include "fboss/agent/state/Interface.h"
#include "fboss/agent/test/EcmpSetupHelper.h"

#include "fboss/agent/hw/test/gen-cpp2/validated_shell_commands_constants.h"

#include "folly/IPAddressV4.h"

namespace {
auto constexpr kEcmpWidth = 4;
}
namespace facebook::fboss {

void HwProdInvariantHelper::setupEcmp() {
  ecmpHelper_ = std::make_unique<utility::HwIpV6EcmpDataPlaneTestUtil>(
      ensemble_, RouterID(0));
  ecmpHelper_->programRoutes(
      kEcmpWidth, std::vector<NextHopWeight>(kEcmpWidth, 1));
}

std::vector<PortDescriptor> HwProdInvariantHelper::getUplinksForEcmp(
    const int uplinkCount) {
  auto hwSwitch = ensemble_->getHwSwitch();
  auto uplinks =
      utility::getAllUplinkDownlinkPorts(
          hwSwitch, initialConfig(), uplinkCount, is_mmu_lossless_mode())
          .first;

  std::vector<PortDescriptor> ecmpPorts;
  for (auto it = uplinks.begin(); it != uplinks.end(); it++) {
    ecmpPorts.push_back(PortDescriptor(*it));
  }
  EXPECT_TRUE(ecmpPorts.size() > 0);
  return ecmpPorts;
}

void HwProdInvariantHelper::setupEcmpWithNextHopMac(
    const folly::MacAddress& nextHopMac) {
  ecmpHelper_ = std::make_unique<utility::HwIpV6EcmpDataPlaneTestUtil>(
      ensemble_, nextHopMac, RouterID(0));

  ecmpPorts_ = getUplinksForEcmp(kEcmpWidth);
  ecmpHelper_->programRoutesVecHelper(
      ecmpPorts_, std::vector<NextHopWeight>(kEcmpWidth, 1));
}

void HwProdInvariantHelper::setupEcmpOnUplinks() {
  ecmpHelper_ = std::make_unique<utility::HwIpV6EcmpDataPlaneTestUtil>(
      ensemble_, RouterID(0));

  ecmpPorts_ = getUplinksForEcmp(kEcmpWidth);
  ecmpHelper_->programRoutesVecHelper(
      ecmpPorts_, std::vector<NextHopWeight>(kEcmpWidth, 1));
}

void HwProdInvariantHelper::sendTraffic() {
  CHECK(ecmpHelper_);
  ecmpHelper_->pumpTrafficThroughPort(getDownlinkPort());
}

/*
 * "On Downlink" really just means "On Not-an-ECMP-port". ECMP is only setup
 * on uplinks, so using a downlink is a safe way to do it.
 */
void HwProdInvariantHelper::sendTrafficOnDownlink() {
  CHECK(ecmpHelper_);
  // This function is mostly used for verifying load balancing, where packets
  // will end up on ECMP-enabled uplinks and then be verified.
  // Since the traffic ends up on uplinks, we send the traffic through
  // downlink.
  auto downlink = getDownlinkPort();
  ecmpHelper_->pumpTrafficThroughPort(downlink);
}

void HwProdInvariantHelper::verifyLoadBalacing() {
  CHECK(ecmpHelper_);
  sendTrafficOnDownlink();
  bool loadBalanced = ecmpHelper_->isLoadBalanced(
      ecmpPorts_, std::vector<NextHopWeight>(kEcmpWidth, 1), 25);
  EXPECT_TRUE(loadBalanced);
}

std::shared_ptr<SwitchState> HwProdInvariantHelper::getProgrammedState() const {
  return ensemble_->getProgrammedState();
}

PortID HwProdInvariantHelper::getDownlinkPort() {
  // pick the first downlink in the list
  return utility::getAllUplinkDownlinkPorts(
             ensemble_->getHwSwitch(),
             initialConfig(),
             kEcmpWidth,
             is_mmu_lossless_mode())
      .second[0];
}

void HwProdInvariantHelper::sendAndVerifyPkts(
    uint16_t destPort,
    uint8_t queueId) {
  auto sendPkts = [this, destPort] {
    auto vlanId = utility::firstVlanID(getProgrammedState());
    auto intf =
        getProgrammedState()->getInterfaces()->getInterfaceInVlan(vlanId);
    auto intfMac = intf->getMac();
    utility::getInterfaceMac(ensemble_->getProgrammedState(), vlanId);
    auto dstIp = intf->getAddresses().begin()->first;
    utility::sendTcpPkts(
        ensemble_->getHwSwitch(),
        1 /*numPktsToSend*/,
        vlanId,
        intfMac,
        dstIp,
        utility::kNonSpecialPort1,
        destPort,
        getDownlinkPort());
  };

  utility::sendPktAndVerifyCpuQueue(
      ensemble_->getHwSwitch(), queueId, sendPkts, 1);
}

void HwProdInvariantHelper::verifyCopp() {
  sendAndVerifyPkts(
      utility::kBgpPort,
      utility::getCoppHighPriQueueId(ensemble_->getPlatform()->getAsic()));
  sendAndVerifyPkts(utility::kNonSpecialPort2, utility::kCoppMidPriQueueId);
}

// two ways to get the ECMP ports
// either they are populated in the ecmpPorts_
// or we pick them from the ecmpHelper
// since we have some tests using both
// support both for now and phase out the one using kEcmpWidth
std::vector<PortID> HwProdInvariantHelper::getEcmpPortIds() {
  std::vector<PortID> ecmpPortIds{};
  for (auto portDesc : ecmpPorts_) {
    EXPECT_TRUE(portDesc.isPhysicalPort());
    auto portId = portDesc.phyPortID();
    ecmpPortIds.emplace_back(portId);
  }

  return ecmpPortIds;
}

void HwProdInvariantHelper::verifyDscpToQueueMapping() {
  if (!ensemble_->getAsic()->isSupported(HwAsic::Feature::L3_QOS)) {
    return;
  }
  auto portStatsBefore =
      ensemble_->getLatestPortStats(ensemble_->masterLogicalPortIds());
  auto vlanId = VlanID(*initialConfig().vlanPorts_ref()[0].vlanID_ref());
  auto intfMac =
      utility::getInterfaceMac(ensemble_->getProgrammedState(), vlanId);

  // Since Olympic QoS is enabled on uplinks (at least on mhnics), we send the
  // packets to be verified on an arbitrary downlink.
  auto downlinkPort = getDownlinkPort();
  auto q2dscpMap = utility::getOlympicQosMaps(initialConfig());
  for (const auto& q2dscps : q2dscpMap) {
    for (auto dscp : q2dscps.second) {
      utility::sendTcpPkts(
          ensemble_->getHwSwitch(),
          1 /*numPktsToSend*/,
          vlanId,
          intfMac,
          folly::IPAddressV6("2620:0:1cfe:face:b00c::4"), // dst ip
          8000,
          8001,
          downlinkPort,
          dscp);
    }
  }
  bool mappingVerified = false;
  auto portIds = getEcmpPortIds();
  for (auto portId : portIds) {
    // Since we don't know which port the above IP will get hashed to,
    // iterate over all ports in ecmp group to find one which satisfies
    // dscp to queue mapping.
    if (mappingVerified) {
      break;
    }
    mappingVerified = utility::verifyQueueMappings(
        portStatsBefore[portId], q2dscpMap, ensemble_, portId);
  }
  EXPECT_TRUE(mappingVerified);
}

void HwProdInvariantHelper::verifySafeDiagCmds() {
  std::set<std::string> diagCmds;
  switch (ensemble_->getAsic()->getAsicType()) {
    case HwAsic::AsicType::ASIC_TYPE_FAKE:
    case HwAsic::AsicType::ASIC_TYPE_MOCK:
    case HwAsic::AsicType::ASIC_TYPE_TAJO:
    case HwAsic::AsicType::ASIC_TYPE_ELBERT_8DD:
      break;

    case HwAsic::AsicType::ASIC_TYPE_TRIDENT2:
      diagCmds = validated_shell_commands_constants::TD2_TESTED_CMDS();
    case HwAsic::AsicType::ASIC_TYPE_TOMAHAWK:
      diagCmds = validated_shell_commands_constants::TH_TESTED_CMDS();
    case HwAsic::AsicType::ASIC_TYPE_TOMAHAWK3:
      diagCmds = validated_shell_commands_constants::TH3_TESTED_CMDS();
    case HwAsic::AsicType::ASIC_TYPE_TOMAHAWK4:
      diagCmds = validated_shell_commands_constants::TH4_TESTED_CMDS();
      break;
  }
  if (diagCmds.size()) {
    for (auto i = 0; i < 10; ++i) {
      for (auto cmd : diagCmds) {
        std::string out;
        ensemble_->runDiagCommand(cmd + "\n", out);
      }
    }
    std::string out;
    ensemble_->runDiagCommand("quit\n", out);
  }
}

void HwProdInvariantHelper::verifyNoDiscards() {
  auto portId = ensemble_->masterLogicalPortIds()[0];
  auto outDiscards = *ensemble_->getLatestPortStats(portId).outDiscards__ref();
  auto inDiscards = *ensemble_->getLatestPortStats(portId).inDiscards__ref();
  EXPECT_EQ(outDiscards, 0);
  EXPECT_EQ(inDiscards, 0);
}

void HwProdInvariantHelper::disableTtl() {
  for (const auto& nhop : ecmpHelper_->getNextHops()) {
    if (std::find(ecmpPorts_.begin(), ecmpPorts_.end(), nhop.portDesc) !=
        ecmpPorts_.end()) {
      utility::disableTTLDecrements(
          ensemble_->getHwSwitch(), RouterID(0), nhop);
    }
  }
}

void HwProdInvariantHelper::verifyQueuePerHostMapping() {
  auto vlanId = utility::firstVlanID(getProgrammedState());
  auto intfMac =
      utility::getInterfaceMac(ensemble_->getProgrammedState(), vlanId);
  auto srcMac = utility::MacAddressGenerator().get(intfMac.u64NBO());

  utility::verifyQueuePerHostMapping(
      ensemble_->getHwSwitch(),
      ensemble_,
      vlanId,
      srcMac,
      intfMac,
      folly::IPAddressV4("1.0.0.1"),
      folly::IPAddressV4("10.10.1.2"),
      true /* useFrontPanel */,
      false /* blockNeighbor */);
}

} // namespace facebook::fboss
