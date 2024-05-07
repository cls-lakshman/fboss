// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include <folly/String.h>
#include <folly/Unit.h>
#include <string>

namespace thriftpath {

#define STRUCT_CHILD_GETTERS(child, childId)                                 \
  TypeFor<strings::child> child() const& {                                   \
    return TypeFor<strings::child>(                                          \
        copyAndExtendVec(this->tokens_, #child),                             \
        copyAndExtendVec(this->idTokens_, folly::to<std::string>(childId))); \
  }                                                                          \
  TypeFor<strings::child> child()&& {                                        \
    this->tokens_.push_back(#child);                                         \
    this->idTokens_.push_back(folly::to<std::string>(childId));              \
    return TypeFor<strings::child>(                                          \
        std::move(this->tokens_), std::move(this->idTokens_));               \
  }
#define CONTAINER_CHILD_GETTERS(key_type)                               \
  Child operator[](key_type token) const& {                             \
    const std::string strToken = folly::to<std::string>(token);         \
    return Child(                                                       \
        copyAndExtendVec(this->tokens_, strToken),                      \
        copyAndExtendVec(this->idTokens_, strToken));                   \
  }                                                                     \
  Child operator[](key_type token)&& {                                  \
    const std::string strToken = folly::to<std::string>(token);         \
    this->tokens_.push_back(strToken);                                  \
    this->idTokens_.push_back(strToken);                                \
    return Child(std::move(this->tokens_), std::move(this->idTokens_)); \
  }

class BasePath {
 public:
  BasePath(std::vector<std::string> tokens, std::vector<std::string> idTokens)
      : tokens_(std::move(tokens)), idTokens_(std::move(idTokens)) {}

  auto begin() const {
    return tokens_.cbegin();
  }

  auto end() const {
    return tokens_.cend();
  }

  const std::vector<std::string>& tokens() const {
    return tokens_;
  }

  const std::vector<std::string>& idTokens() const {
    return idTokens_;
  }

  bool matchesPath(const std::vector<std::string>& other) const {
    return other == idTokens_ || other == tokens_;
  }

  std::string str() const {
    // TODO: better format
    return "/" + folly::join('/', tokens_.begin(), tokens_.end());
  }

 protected:
  std::vector<std::string> tokens_;
  std::vector<std::string> idTokens_;
};

template <
    typename _DataT,
    typename _RootT,
    typename _TC,
    typename _Tag,
    typename _ParentT>
class Path : public BasePath {
 public:
  using DataT = _DataT;
  using RootT = _RootT;
  using TC = _TC;
  using Tag = _Tag;
  using ParentT = _ParentT;

  using BasePath::BasePath;
};

template <typename T>
class RootThriftPath {
 public:
  // While this is always-false, it is dependent and therefore fires only
  // at instantiation time.
  static_assert(
      !std::is_same<T, T>::value,
      "You need to include the header file that the thriftpath plugin "
      "generated for T in order to use RootThriftPath<T>. Also ensure that "
      "you have annotated your root struct with (thriftpath.root)");
};

template <typename T, typename Root, typename Parent>
class ChildThriftPath {
 public:
  // While this is always-false, it is dependent and therefore fires only
  // at instantiation time.
  static_assert(
      !std::is_same<T, T>::value,
      "You need to include the header file that the thriftpath plugin "
      "generated for T in order to use ChildThriftPath<T>.");
};

std::vector<std::string> copyAndExtendVec(
    const std::vector<std::string>& parents,
    std::string last);

} // namespace thriftpath
