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

#include <cstdint>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

int main() {
    // 8-byte cell: works on every supported compiler/target.
    mpmc::mpmc_queue<std::uint32_t, 8, std::uint32_t> queue;
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (!queue.try_push(i)) {
            return 1;
        }
    }
    if (queue.try_push(99)) {
        return 2;
    }
    for (std::uint32_t i = 0; i < 8; ++i) {
        std::uint32_t value = 0;
        if (!queue.try_pop(value) || value != i) {
            return 3;
        }
    }
    if (!queue.empty()) {
        return 4;
    }

    mpmc::spsc_queue<std::uint32_t, 8, std::uint32_t> spsc;
    for (std::uint32_t i = 0; i < 8; ++i) {
        if (!spsc.try_push(i)) {
            return 10;
        }
    }
    for (std::uint32_t i = 0; i < 8; ++i) {
        std::uint32_t value = 0;
        if (!spsc.try_pop(value) || value != i) {
            return 11;
        }
    }
    if (!spsc.empty()) {
        return 12;
    }

#if !defined(_WIN32)
    // Exercise the shared-memory adapter without adding Threads::Threads or
    // any other dependency manually: the imported mpmc::mpmc_queue target and
    // the installed package config must provide them.
    using shared_type = mpmc::shared_memory_queue<std::uint32_t, std::uint32_t>;
    const std::string name =
        "/mpmc_consumer_" + std::to_string(static_cast<long long>(::getpid()));
    std::error_code error;
    {
        auto owner = shared_type::create(
            name, 4, mpmc::shared_memory_lifetime::unlink_on_destroy, error);
        if (error || !owner) {
            return 5;
        }
        if (!owner->try_push(42)) {
            return 6;
        }
        auto attacher = shared_type::attach(name, error);
        if (error || !attacher) {
            return 7;
        }
        std::uint32_t value = 0;
        if (!attacher->try_pop(value) || value != 42) {
            return 8;
        }
    }
    if (shared_type::exists(name, error) || error) {
        return 9;
    }
#endif
    return 0;
}
