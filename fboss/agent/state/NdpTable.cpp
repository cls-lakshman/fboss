/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "fboss/agent/state/NdpTable.h"

#include "fboss/agent/state/NeighborTable-defs.h"

namespace facebook::fboss {

using NdpTableTraits = NeighborTableTraits<folly::IPAddressV6, NdpEntry>;
template class NeighborTable<folly::IPAddressV6, NdpEntry, NdpTable>;

} // namespace facebook::fboss
