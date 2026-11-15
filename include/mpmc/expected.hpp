// SPDX-FileCopyrightText: 2026 Mohammad Mahdi Abdollahpour <git@mohammadmahdi.com>
// SPDX-License-Identifier: CPAL-1.0
//
// The contents of this file are subject to the Common Public Attribution License Version 1.0 (the "License"); you may not use this file except in compliance with the License. You may obtain a copy of the License at https://opensource.org/license/cpal-1-0. The License is based on the Mozilla Public License Version 1.1 but Sections 14 and 15 have been added to cover use of software over a computer network and provide for limited attribution for the Original Developer. In addition, Exhibit A has been modified to be consistent with Exhibit B.
// Software distributed under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License for the specific language governing rights and limitations under the License.
// The Original Code is mpmc_queue, a header-only C++20 bounded queue library.
// The Original Developer is not separately identified and is the Initial Developer. If left blank, the Original Developer is the Initial Developer.
// The Initial Developer of the Original Code is Mohammad Mahdi Abdollahpour. All portions of the code written by Mohammad Mahdi Abdollahpour are Copyright (c) 2026. All Rights Reserved.
// Contributor: Mohammad Mahdi Abdollahpour.
//
// Optional C++23 std::expected integration.  The core library remains C++20;
// this header exposes the opt-in expected-returning setup APIs only when the
// standard library provides std::expected.

#pragma once

#include <system_error>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L &&            \
    __has_include(<expected>)
#include <expected>
#define MPMC_HAS_EXPECTED 1
#else
#define MPMC_HAS_EXPECTED 0
#endif

namespace mpmc {

#if MPMC_HAS_EXPECTED
template<class T, class E = std::error_code>
using expected = std::expected<T, E>;

inline constexpr bool expected_available = true;
#else
inline constexpr bool expected_available = false;
#endif

} // namespace mpmc
