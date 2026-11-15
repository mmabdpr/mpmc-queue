// SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
// SPDX-License-Identifier: CPAL-1.0
//
// The contents of this file are subject to the Common Public Attribution
// License Version 1.0 (the "License"); you may not use this file except in
// compliance with the License. You may obtain a copy of the License at
// https://opensource.org/license/cpal-1-0.
// The License is based on the Mozilla Public License Version 1.1 but
// Sections 14 and 15 have been added to cover use of software over a
// computer network and provide for limited attribution for the Original
// Developer. In addition, Exhibit A has been modified to be consistent
// with Exhibit B.
//
// Software distributed under the License is distributed on an "AS IS"
// basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
// License for the specific language governing rights and limitations
// under the License.
//
// The Original Code is mpmc_queue, a header-only C++20 bounded queue
// library.
// The Original Developer is not separately identified and is the Initial
// Developer. If left blank, the Original Developer is the Initial
// Developer.
// The Initial Developer of the Original Code is Mohammad Mahdi
// Abdollahpour. All portions of the code written by Mohammad Mahdi
// Abdollahpour are Copyright (c) 2026. All Rights Reserved.
// Contributor: Mohammad Mahdi Abdollahpour.
//
// Public constraints for queue payloads, indices, and capacities.

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace mpmc {

namespace detail {

template<class T>
concept complete_object = std::is_object_v<T> && requires { sizeof(T); };

template<class T, class Index> struct payload_fits : std::false_type {};

template<class T, class Index>
    requires complete_object<T> && requires { sizeof(Index); }
struct payload_fits<T, Index>
    : std::bool_constant<(sizeof(T) + sizeof(Index) <= 16)> {};

} // namespace detail

/// An index must be an unqualified unsigned integer of 1, 2, 4, or 8 bytes.
/// The marker arithmetic relies on an unsigned type and on the marker horizon
/// fitting in the selected width.
template<class Index>
concept supported_index = requires {
    requires std::same_as<Index, std::remove_cv_t<Index>>;
    requires std::unsigned_integral<Index>;
    requires !std::same_as<Index, bool>;
    requires(sizeof(Index) == 1 || sizeof(Index) == 2 || sizeof(Index) == 4 ||
             sizeof(Index) == 8);
};

/// A value that can be copied into an atomic cell: a complete, trivially
/// copyable, non-cv object.
template<class T>
concept queue_payload =
    detail::complete_object<T> && std::is_trivially_copyable_v<T> &&
    !std::is_const_v<T> && !std::is_volatile_v<T>;

/// A payload and index combination whose bytes fit in one 8- or 16-byte cell.
template<class T, class Index>
concept queue_payload_for = queue_payload<T> && supported_index<Index> &&
                            detail::payload_fits<T, Index>::value;

/// A compile-time capacity: zero selects runtime sizing; otherwise the value
/// must be a power of two no larger than numeric_limits<Index>::max() / 4.
template<std::size_t Capacity, class Index>
concept valid_static_capacity =
    supported_index<Index> &&
    (Capacity == 0 || (std::has_single_bit(Capacity) &&
                       Capacity <= static_cast<std::size_t>(
                                       std::numeric_limits<Index>::max() / 4)));

/// A non-array object type that can be owned by std::unique_ptr.
template<class T>
concept unique_ptr_payload = std::is_object_v<T> && !std::is_array_v<T>;

} // namespace mpmc
