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
#include "fboss/agent/platforms/common/PlatformMapping.h"
#include "fboss/lib/platforms/PlatformMode.h"

namespace facebook::fboss::utility {
std::unique_ptr<PlatformMapping> initPlatformMapping(PlatformType mode);
} // namespace facebook::fboss::utility
