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
#include <mpmc/mpmc.hpp>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    constexpr std::uint32_t producer_count = 4;
    constexpr std::uint32_t consumer_count = 4;
    constexpr std::uint32_t per_producer = 100000;
    constexpr std::uint32_t total = producer_count * per_producer;

    mpmc::mpmc_queue<std::uint32_t, 1024, std::uint32_t> queue;
    std::atomic<std::uint32_t> consumed{0};

    std::vector<std::thread> threads;
    threads.reserve(producer_count + consumer_count);
    for (std::uint32_t p = 0; p < producer_count; ++p) {
        threads.emplace_back([&queue, p] {
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                const std::uint32_t value = (std::uint32_t{p} << 24U) | i;
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::uint32_t c = 0; c < consumer_count; ++c) {
        threads.emplace_back([&queue, &consumed] {
            for (;;) {
                std::uint32_t value = 0;
                if (queue.try_pop(value)) {
                    if (consumed.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                        total) {
                        return;
                    }
                } else if (consumed.load(std::memory_order_acquire) >= total) {
                    return;
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::cout << "consumed " << consumed.load() << " values\n";
    return consumed.load() == total ? 0 : 1;
}
