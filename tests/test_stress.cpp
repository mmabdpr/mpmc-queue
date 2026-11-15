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
#include "mpmc/mpmc.hpp"

#include "test_cell_config.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace {

using cell_value = mpmc_test::wide_value;
using cell_index = mpmc_test::wide_index;

using clock_type = std::chrono::steady_clock;

[[nodiscard]] bool expired(clock_type::time_point deadline) noexcept {
    return clock_type::now() > deadline;
}

struct observed {
    cell_index sequence;
    cell_value value;
};

} // namespace

MPMC_TEST(mpmc_stress_no_loss_no_duplication) {
    constexpr std::uint32_t producer_count = 4;
    constexpr std::uint32_t consumer_count = 4;
    constexpr std::uint32_t per_producer = 20000;
    constexpr std::uint64_t total =
        static_cast<std::uint64_t>(producer_count) * per_producer;
    static_assert(per_producer < (std::uint32_t{1} << 24U));

    mpmc::mpmc_queue<cell_value, 256, cell_index> queue;
    std::vector<std::atomic<bool>> published(total);
    for (auto& flag : published) {
        flag.store(false, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> failed{false};
    std::vector<std::vector<observed>> observations(consumer_count);

    const auto deadline = clock_type::now() + std::chrono::seconds(30);

    auto producer = [&](std::uint32_t id) {
        try {
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                const cell_value value = static_cast<cell_value>(
                    (static_cast<std::uint32_t>(id) << 24U) | i);
                cell_index sequence = 0;
                while (!queue.try_push(value, sequence)) {
                    if (failed.load(std::memory_order_acquire) ||
                        expired(deadline)) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
                if (static_cast<std::uint64_t>(sequence) >= total) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                published[static_cast<std::size_t>(sequence)].store(
                    true, std::memory_order_release);
                produced.fetch_add(1, std::memory_order_release);
            }
        } catch (...) {
            failed.store(true, std::memory_order_release);
        }
    };

    auto consumer = [&](std::uint32_t id) {
        try {
            std::vector<observed>& local = observations[id];
            local.reserve(per_producer * producer_count / consumer_count + 16);
            for (;;) {
                cell_value value = 0;
                cell_index sequence = 0;
                if (queue.try_pop(value, sequence)) {
                    local.push_back(observed{sequence, value});
                    const std::uint64_t count =
                        consumed.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (count == total) {
                        return;
                    }
                } else if (produced.load(std::memory_order_acquire) == total &&
                           queue.empty()) {
                    return;
                } else {
                    if (failed.load(std::memory_order_acquire) ||
                        expired(deadline)) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        } catch (...) {
            failed.store(true, std::memory_order_release);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(producer_count + consumer_count);
    for (std::uint32_t i = 0; i < producer_count; ++i) {
        threads.emplace_back(producer, i);
    }
    for (std::uint32_t i = 0; i < consumer_count; ++i) {
        threads.emplace_back(consumer, i);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK_EQ(consumed.load(std::memory_order_acquire), total);
    CHECK_EQ(produced.load(std::memory_order_acquire), total);
    CHECK(queue.empty());

    std::size_t observed_count = 0;
    for (const auto& local : observations) {
        observed_count += local.size();
        for (std::size_t i = 1; i < local.size(); ++i) {
            CHECK(local[i - 1].sequence < local[i].sequence);
        }
    }
    CHECK_EQ(observed_count, static_cast<std::size_t>(total));

    std::vector<observed> all;
    all.reserve(static_cast<std::size_t>(total));
    for (const auto& local : observations) {
        all.insert(all.end(), local.begin(), local.end());
    }
    std::sort(all.begin(), all.end(), [](const observed& a, const observed& b) {
        return a.sequence < b.sequence;
    });

    REQUIRE_EQ(all.size(), static_cast<std::size_t>(total));
    std::vector<bool> seen(static_cast<std::size_t>(total), false);
    std::vector<std::int64_t> last_value(producer_count, -1);
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK_EQ(static_cast<std::uint64_t>(all[i].sequence),
                 static_cast<std::uint64_t>(i));
        if (static_cast<std::uint64_t>(all[i].sequence) < total) {
            CHECK(!seen[static_cast<std::size_t>(all[i].sequence)]);
            seen[static_cast<std::size_t>(all[i].sequence)] = true;
        }

        const std::uint32_t producer_id =
            static_cast<std::uint32_t>(all[i].value >> 24U);
        const std::uint32_t item =
            static_cast<std::uint32_t>(all[i].value & 0x00ffffffU);
        REQUIRE(producer_id < producer_count);
        CHECK(item < per_producer);
        CHECK(static_cast<std::int64_t>(item) > last_value[producer_id]);
        last_value[producer_id] = static_cast<std::int64_t>(item);
    }

    for (const auto& flag : published) {
        CHECK(flag.load(std::memory_order_acquire));
    }
    for (bool flag : seen) {
        CHECK(flag);
    }
}

MPMC_TEST(tiny_capacity_mpmc) {
    constexpr std::uint32_t thread_count = 4;
    constexpr std::uint32_t per_thread = 5000;
    constexpr std::uint32_t total = thread_count * per_thread;

    mpmc::mpmc_queue<cell_value, 2, cell_index> queue;
    std::vector<std::atomic<std::uint32_t>> seen(total);
    for (auto& count : seen) {
        count.store(0, std::memory_order_relaxed);
    }
    std::atomic<std::uint32_t> consumed{0};
    std::atomic<bool> failed{false};
    const auto deadline = clock_type::now() + std::chrono::seconds(30);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(thread_count) * 2);
    for (std::uint32_t p = 0; p < thread_count; ++p) {
        threads.emplace_back([&, p] {
            for (std::uint32_t i = 0; i < per_thread; ++i) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                const cell_value value = static_cast<cell_value>(
                    static_cast<std::uint64_t>(p) * per_thread + i);
                while (!queue.try_push(value)) {
                    if (failed.load(std::memory_order_acquire) ||
                        expired(deadline)) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::uint32_t c = 0; c < thread_count; ++c) {
        threads.emplace_back([&] {
            for (;;) {
                cell_value value = 0;
                if (queue.try_pop(value)) {
                    if (static_cast<std::uint32_t>(value) >= total) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    seen[static_cast<std::size_t>(value)].fetch_add(
                        1, std::memory_order_relaxed);
                    const std::uint32_t count =
                        consumed.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (count == total) {
                        return;
                    }
                } else if (consumed.load(std::memory_order_acquire) >= total) {
                    return;
                } else {
                    if (failed.load(std::memory_order_acquire) ||
                        expired(deadline)) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK_EQ(consumed.load(std::memory_order_acquire), total);
    for (const auto& count : seen) {
        CHECK_EQ(count.load(std::memory_order_acquire), std::uint32_t{1});
    }
    CHECK(queue.empty());
}

MPMC_TEST(single_producer_single_consumer_order) {
    constexpr std::uint64_t count = 100000;
    mpmc::mpmc_queue<cell_value, 64, cell_index> queue;

    std::atomic<bool> failed{false};
    const auto deadline = clock_type::now() + std::chrono::seconds(30);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            if (failed.load(std::memory_order_acquire)) {
                return;
            }
            while (!queue.try_push(static_cast<cell_value>(i))) {
                if (failed.load(std::memory_order_acquire) ||
                    expired(deadline)) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    cell_value expected = 0;
    while (expected < static_cast<cell_value>(count)) {
        cell_value value = 0;
        cell_index sequence = 0;
        if (queue.try_pop(value, sequence)) {
            if (value != expected ||
                static_cast<cell_value>(sequence) != expected) {
                failed.store(true, std::memory_order_release);
                break;
            }
            ++expected;
        } else {
            if (expired(deadline)) {
                failed.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
    }
    producer.join();

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK_EQ(static_cast<std::uint64_t>(expected), count);
    CHECK(queue.empty());
}
