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
#include <optional>
#include <set>
#include <vector>

#include "fboss/lib/fpga/MultiPimPlatformPimContainer.h"

namespace facebook::fboss {

class PhyManager;
class MultiPimPlatformMapping;

/*
 * HwPhyEnsemble will be the hw_test ensemble class to manager PhyManager.
 * This ensemble will be used in qsfp_service phy related hw_test to provide
 * all phy related helper function accross different phy implementation.
 * Currently we have three major implementations of PhyManager:
 * - Broadcom XPHY (Minipack1 and Fuji)
 * - Yamp XPHY
 * - SAI XPHY (Elbert)
 * This base class can provide the definition of all the phy related helper
 * functions, and then all the above three implementation can extend
 * HwPhyEnsemble to implement these functions in their own way with their own
 * PhyManager.
 * So the directory of phy related hw_test will be:
 * - phy
 * --- HwPhyEnsemble.h
 * --- sai
 * ----- SaiPhyEnsemble.h
 * --- bcm
 * ----- BcmPhyEnsemble.h
 * --- yamp
 * ----- YampPhyEnsemble.h
 */
class HwPhyEnsemble {
 public:
  struct HwPhyEnsembleInitInfo {
    // Specify which pim type the HwTest want to run all the tests against.
    // We have some platforms using two different pim types, for example,
    // Elbert uses Elbert16Q and Elbert8DD, and these linecards have different
    // phy features. So we need user to define which linecard type to use
    // before running the hw_test.
    MultiPimPlatformPimContainer::PimType pimType;
  };

  HwPhyEnsemble();
  // TODO(joseph5wu) Might need some extra logic to handle desctrutor
  virtual ~HwPhyEnsemble();

  virtual void init(const HwPhyEnsembleInitInfo& info);

  PhyManager* getPhyManager() const {
    return phyManager_.get();
  }

  int8_t getTargetPimID() const {
    return targetPimID_;
  }

 protected:
  std::vector<int> getTargetPimXphyList(
      MultiPimPlatformMapping* platformMapping) const;

  std::unique_ptr<PhyManager> phyManager_;
  std::unique_ptr<MultiPimPlatformMapping> platformMapping_;
  int8_t targetPimID_{-1};

 private:
  virtual std::unique_ptr<PhyManager> choosePhyManager(
      MultiPimPlatformPimContainer::PimType pimType) = 0;

  virtual std::unique_ptr<MultiPimPlatformMapping>
  chooseMultiPimPlatformMapping(
      MultiPimPlatformPimContainer::PimType pimType) = 0;
  // Based on pimType to find the first available pim ID.
  // Will throw exception if such pimType doesn't exist
  int8_t getFirstAvailablePimID(MultiPimPlatformPimContainer::PimType pimType);
};
} // namespace facebook::fboss
