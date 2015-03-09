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

#include <folly/Range.h>
#include <memory>

namespace facebook { namespace fboss {

namespace cfg {
class SwitchConfig;
}

class Platform;
class SwitchState;

/*
 * Apply a thrift config structure to a SwitchState object.
 *
 * Returns a new SwitchState object with the resulting state, or null if
 * the config file results in no changes.
 */
std::shared_ptr<SwitchState> applyThriftConfig(
    const std::shared_ptr<SwitchState>& state,
    const cfg::SwitchConfig* config,
    const Platform* platform);
std::shared_ptr<SwitchState> applyThriftConfigFile(
    const std::shared_ptr<SwitchState>& state,
    folly::StringPiece path,
    const Platform* platform);
std::shared_ptr<SwitchState> applyThriftConfigDefault(
    std::shared_ptr<SwitchState> state,
    const Platform* platform);

}} // facebook::fboss
