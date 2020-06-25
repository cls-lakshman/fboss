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

#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/MacAddress.h>

#include <type_traits>
#include <variant>

extern "C" {
#include <sai.h>
}

#include <boost/functional/hash.hpp>
#include "fboss/lib/TupleUtils.h"

namespace facebook::fboss {

/*
 * Helper metafunctions for C++ wrappers of non-primitive SAI types.
 *
 * e.g., folly::MacAddress for sai_mac_t, folly::IPAddress for
 * sai_ip_address_t, std::vector<uint64_t> for sai_object_list_t
 */
template <typename T>
struct WrappedSaiType {
  using value = T;
};

template <>
struct WrappedSaiType<folly::MacAddress> {
  using value = sai_mac_t;
};

template <>
struct WrappedSaiType<folly::IPAddressV4> {
  using value = sai_ip4_t;
};

template <>
struct WrappedSaiType<folly::IPAddressV6> {
  using value = sai_ip6_t;
};

template <>
struct WrappedSaiType<folly::IPAddress> {
  using value = sai_ip_address_t;
};

template <>
struct WrappedSaiType<folly::CIDRNetwork> {
  using value = sai_ip_prefix_t;
};

template <>
struct WrappedSaiType<std::vector<sai_object_id_t>> {
  using value = sai_object_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_int8_t>> {
  using value = sai_s8_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_uint8_t>> {
  using value = sai_u8_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_int16_t>> {
  using value = sai_s16_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_uint16_t>> {
  using value = sai_u16_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_int32_t>> {
  using value = sai_s32_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_uint32_t>> {
  using value = sai_u32_list_t;
};

template <>
struct WrappedSaiType<std::vector<sai_qos_map_t>> {
  using value = sai_qos_map_list_t;
};

template <typename T>
class AclEntryField {
 public:
  AclEntryField(){};
  AclEntryField(T dataAndMask) : dataAndMask_(dataAndMask) {}
  T getDataAndMask() const {
    return dataAndMask_;
  }

  void setDataAndMask(T dataAndMask) {
    dataAndMask_ = dataAndMask;
  }

  std::string str() const {
    return folly::to<std::string>(
        "data: ", dataAndMask_.first, " mask: ", dataAndMask_.second);
  }

 private:
  T dataAndMask_;
};

template <typename T>
struct WrappedSaiType<AclEntryField<T>> {
  using value = sai_acl_field_data_t;
};

template <typename T>
bool operator==(const AclEntryField<T>& lhs, const AclEntryField<T>& rhs) {
  return lhs.getDataAndMask() == rhs.getDataAndMask();
}

template <typename T>
std::size_t hash_value(const AclEntryField<T>& key) {
  std::size_t seed = 0;
  boost::hash_combine(seed, boost::hash_value(key.getDataAndMask()));
  return seed;
}

/*
 * AclEntryField's data and mask always have the same data type:
 * Packet matching takes into account only the bits in the data which have a
 * corresponding '1' in the same position of the mask. Bits in the data which
 * have corresponding '0' bit in the same position of the mask are ignored.
 */
using AclEntryFieldU8 = AclEntryField<std::pair<sai_uint8_t, sai_uint8_t>>;
using AclEntryFieldU16 = AclEntryField<std::pair<sai_uint16_t, sai_uint16_t>>;
using AclEntryFieldU32 = AclEntryField<std::pair<sai_uint32_t, sai_uint32_t>>;
using AclEntryFieldIpV6 =
    AclEntryField<std::pair<folly::IPAddressV6, folly::IPAddressV6>>;

template <typename T>
struct IsSaiTypeWrapper
    : std::negation<std::is_same<typename WrappedSaiType<T>::value, T>> {};

/*
 * Helper metafunctions for resolving two types in the SAI
 * sai_attribute_value_t union being aliases. This results in SaiAttribute
 * not being able to select the correct union member from sai_attribute_t
 * from the SAI type alone. i.e., how could it choose between attr.value.oid
 * and attr.value.u64 based on just the fact that it is extracting a 64 bit
 * unsigned integer?
 *
 * e.g., sai_object_id_t vs uint64_t and sai_ip4_t vs uint32_t;
 */
struct SaiObjectIdT {};

template <typename T>
struct DuplicateTypeFixer {
  using value = T;
};

template <>
struct DuplicateTypeFixer<SaiObjectIdT> {
  using value = sai_object_id_t;
};

template <>
struct DuplicateTypeFixer<folly::IPAddressV4> {
  using value = sai_ip4_t;
};

template <typename T>
struct IsDuplicateSaiType
    : std::negation<std::is_same<typename DuplicateTypeFixer<T>::value, T>> {};

template <typename T>
struct IsSaiAttribute : public std::false_type {};

template <typename AttrT>
struct IsSaiAttribute<std::optional<AttrT>> : public IsSaiAttribute<AttrT> {};

template <typename T>
struct IsSaiEntryStruct : public std::false_type {};

template <typename SaiObjectTraits>
struct AdapterKeyIsEntryStruct
    : public IsSaiEntryStruct<typename SaiObjectTraits::AdapterKey> {};

template <typename SaiObjectTraits>
struct AdapterKeyIsObjectId
    : std::negation<AdapterKeyIsEntryStruct<SaiObjectTraits>> {};

template <typename T>
struct IsTupleOfSaiAttributes : public std::false_type {};

template <typename... AttrTs>
struct IsTupleOfSaiAttributes<std::tuple<AttrTs...>>
    : public std::conjunction<IsSaiAttribute<AttrTs>...> {};

template <typename SaiObjectTraits>
struct IsSaiObjectOwnedByAdapter : public std::false_type {};

template <typename SaiObjectTraits>
struct SaiObjectHasStats : public std::false_type {};

template <typename SaiObjectTraits>
struct SaiObjectHasConditionalAttributes : public std::false_type {};

template <typename ObjectTrait>
using AdapterHostKeyTrait = typename ObjectTrait::AdapterHostKey;

/*
 * For a condition object trait, define adapter host key type
 * Adapter host key type for condition object trait is a variant of all object
 * traits forming a condition object trait
 */
template <typename... ConditionObjectTraits>
struct ConditionAdapterHostKeyTraits {
  static_assert(
      (... && SaiObjectHasConditionalAttributes<ConditionObjectTraits>::value),
      "non condition object trait can not use ConditionAdapterHostKeyTraits");
  using AdapterHostKey =
      typename std::variant<AdapterHostKeyTrait<ConditionObjectTraits>...>;
};

/*
 * For a condition object trait, define adapter key type
 * Adapter key type for condition object trait is will remain of one type since
 * each object trait member of condition object trait uses same SAI API.
 */
template <typename AdapterKeyType, typename... ConditionObjectTraits>
struct ConditionAdapterKeyTraits {
  static_assert(
      (... && SaiObjectHasConditionalAttributes<ConditionObjectTraits>::value),
      "non condition object trait can not use ConditionAdapterHostKeyTraits");
  using AdapterKey = AdapterKeyType;
};

/*
 * Condition object trait is a trait for an object which uses condition
 * attribute. a condition attribute leads to more than one object traits for
 * same object api. a typical example is next hop api, which needs condition
 * object trait. since each condition object trait is composed of more than one
 * object traits below traits gives a definition of this composition.
 */
template <typename... ObjectTraits>
struct ConditionObjectTraits {
  static_assert(
      (... && SaiObjectHasConditionalAttributes<ObjectTraits>::value),
      "non condition object trait can not use can not use  on ConditionObjectTraits");
  using ObjectTrait = std::tuple<ObjectTraits...>;
  static auto constexpr ObjectTraitCount = sizeof...(ObjectTraits);
  using ConditionAttributes = std::remove_const_t<
      typename std::tuple_element_t<0, ObjectTrait>::ConditionAttributes>;
  using AdapterHostKey =
      typename ConditionAdapterHostKeyTraits<ObjectTraits...>::AdapterHostKey;
  template <typename AdapterKeyType>
  using AdapterKey =
      typename ConditionAdapterKeyTraits<AdapterKeyType, ObjectTraits...>::
          AdapterKey;
};
} // namespace facebook::fboss
