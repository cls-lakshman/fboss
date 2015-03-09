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

#include <stdexcept>
#include <string>

namespace facebook { namespace fboss {

/*
 * This exception is thrown by header parsing code.
 */
class HdrParseError : public std::runtime_error {
 public:
  explicit HdrParseError(const std::string& what_arg)
    : std::runtime_error(what_arg) {}
  explicit HdrParseError(const char* what_arg)
    : std::runtime_error(what_arg) {}
};

}} // facebook::fboss
