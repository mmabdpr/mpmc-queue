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
#include <mpmc/mpmc.hpp>

#include "test_cell_config.hpp"

#include <cstdint>
#include <string>
#include <system_error>
#include <type_traits>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if MPMC_HAS_EXPECTED
using expected_queue =
    mpmc::mpmc_queue<mpmc_test::wide_value, 8, mpmc_test::wide_index>;

struct throwing_constructor {
    throwing_constructor() noexcept(false) {}
};

template<class Queue>
concept make_queue_available =
    requires(std::error_code& error) { mpmc::make_queue<Queue>(error); };

template<class Queue>
concept make_queue_expected_available =
    requires { mpmc::make_queue_expected<Queue>(); };

static_assert(make_queue_available<expected_queue>);
static_assert(make_queue_expected_available<expected_queue>);
static_assert(!make_queue_available<throwing_constructor>);
static_assert(!make_queue_expected_available<throwing_constructor>);
#endif

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
#if !MPMC_HAS_EXPECTED
    return 77;
#else
    using value_type = mpmc_test::wide_value;
    using index_type = mpmc_test::wide_index;
    using queue_type = mpmc::mpmc_queue<value_type, 8, index_type>;
    static_assert(mpmc::expected_available);
    static_assert(
        std::is_same_v<
            decltype(queue_type::create_expected()),
            mpmc::expected<std::unique_ptr<queue_type>, std::error_code>>);

    auto fixed = queue_type::create_expected();
    if (!fixed || !(*fixed)->try_push(value_type{1})) {
        return 1;
    }
    auto runtime =
        mpmc::mpmc_queue<value_type, 0, index_type>::create_expected(8);
    if (!runtime || !(*runtime)->try_push(value_type{2})) {
        return 2;
    }
    auto heap = mpmc::make_queue_expected<queue_type>();
    if (!heap || !(*heap)->try_push(value_type{3})) {
        return 3;
    }
#if MPMC_TEST_HAS_16_BYTE_CELL
    auto owning = mpmc::unique_ptr_queue<int, 8>::create_expected();
    auto owning_runtime = mpmc::unique_ptr_queue<int>::create_expected(8);
    if (!owning || !owning_runtime) {
        return 4;
    }
#endif

    auto invalid =
        mpmc::mpmc_queue<value_type, 0, index_type>::create_expected(3);
    if (invalid || invalid.error() != mpmc::errc::invalid_capacity) {
        return 5;
    }

    using spsc_fixed = mpmc::spsc_queue<std::uint32_t, 8, std::uint32_t>;
    using spsc_runtime = mpmc::spsc_queue<std::uint32_t, 0, std::uint32_t>;
    static_assert(
        std::is_same_v<
            decltype(spsc_fixed::create_expected()),
            mpmc::expected<std::unique_ptr<spsc_fixed>, std::error_code>>);
    auto spsc = spsc_fixed::create_expected();
    if (!spsc || !(*spsc)->try_push(std::uint32_t{4})) {
        return 9;
    }
    auto spsc_dynamic = spsc_runtime::create_expected(8);
    std::uint32_t spsc_value = 0;
    if (!spsc || !(*spsc)->try_pop(spsc_value) || spsc_value != 4 ||
        !spsc_dynamic || !(*spsc_dynamic)->try_push(std::uint32_t{5})) {
        return 10;
    }

#if !defined(_WIN32)
    using shared_type = mpmc::shared_memory_queue<std::uint32_t, std::uint32_t>;
    const std::string name =
        "/mpmc_expected_" + std::to_string(static_cast<long long>(::getpid()));
    std::error_code cleanup_error;
    [[maybe_unused]] const bool initial_removed =
        shared_type::remove(name, cleanup_error);

    auto shared = shared_type::create_expected(
        name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy);
    if (!shared) {
        return 6;
    }
    auto attached = shared_type::attach_expected(name);
    if (!attached || !(*attached)->try_push(7)) {
        return 7;
    }
    auto exists = shared_type::exists_expected(name);
    if (!exists || !*exists) {
        return 8;
    }
    auto removed = shared_type::remove_expected(name);
    if (!removed || !*removed) {
        return 9;
    }
#endif
    return 0;
#endif
}
