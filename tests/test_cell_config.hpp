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
// Cell-size selection shared by the tests.  CMake probes the actual compiler
// and target and defines MPMC_HAS_LOCK_FREE_16_BYTE_CELL; the header's own
// constexpr probe is the fallback so a standalone compile still selects the
// correct cell.  Tests use wide_value/wide_index so the same source exercises
// an 8-byte cell on targets where 16-byte atomics are not lock-free.

#pragma once

#include <mpmc/detail/cell.hpp>

#include <cstdint>
#include <type_traits>

namespace mpmc_test {

#if defined(MPMC_HAS_LOCK_FREE_16_BYTE_CELL)
inline constexpr bool has_lock_free_16_byte_cell =
    (MPMC_HAS_LOCK_FREE_16_BYTE_CELL != 0);
#else
inline constexpr bool has_lock_free_16_byte_cell =
    mpmc::detail::has_lock_free_16_byte_cell;
#endif

// A build-system report that 16-byte cells work must match the compiler's
// answer; otherwise the suite would instantiate a queue that cannot exist and
// report a confusing static-assert failure.
static_assert(
    !has_lock_free_16_byte_cell || mpmc::detail::has_lock_free_16_byte_cell,
    "CMake reports lock-free 16-byte cells but the compiler disagrees");
static_assert(has_lock_free_16_byte_cell ||
                  mpmc::detail::has_lock_free_8_byte_cell,
              "no lock-free cell size is available for this target");

using wide_value = std::conditional_t<has_lock_free_16_byte_cell, std::uint64_t,
                                      std::uint32_t>;
using wide_index = wide_value;

static_assert(sizeof(wide_value) + sizeof(wide_index) ==
                  (has_lock_free_16_byte_cell ? std::size_t{16}
                                              : std::size_t{8}),
              "wide cell must be the selected hardware cell");

} // namespace mpmc_test

// Preprocessor form for declarations that instantiate unique_ptr_queue or
// other 16-byte-only types (which cannot be hidden behind if constexpr).
#if defined(MPMC_HAS_LOCK_FREE_16_BYTE_CELL) && MPMC_HAS_LOCK_FREE_16_BYTE_CELL
#define MPMC_TEST_HAS_16_BYTE_CELL 1
#else
#define MPMC_TEST_HAS_16_BYTE_CELL 0
#endif
