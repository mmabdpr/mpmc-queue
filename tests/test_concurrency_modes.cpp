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
#include "test_framework.hpp"

#include <mpmc/mpmc_queue.hpp>
#include <mpmc/spsc_queue.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

using value_type = std::uint32_t;
using index_type = std::uint32_t;

constexpr std::size_t kQueueCapacity = 64;
constexpr std::uint32_t kItems = 50'000;

inline void wait_until(const std::atomic<bool>& gate) {
    while (!gate.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

template<class Queue> void run_spsc_ordering() {
    Queue queue;
    std::atomic<bool> go{false};

    std::thread producer([&] {
        wait_until(go);
        for (std::uint32_t i = 0; i < kItems; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        wait_until(go);
        std::uint32_t expected = 0;
        while (expected < kItems) {
            value_type value = 0;
            if (queue.try_pop(value)) {
                CHECK_EQ(value, expected);
                ++expected;
            } else {
                std::this_thread::yield();
            }
        }
    });

    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
}

MPMC_TEST(spsc_alias_and_fifo) {
    mpmc::spsc_queue<value_type, kQueueCapacity, index_type> queue;
    static_assert(decltype(queue)::mode == mpmc::concurrency::spsc);
    static_assert(decltype(queue)::single_producer);
    static_assert(decltype(queue)::single_consumer);

    for (value_type i = 0; i < kQueueCapacity; ++i) {
        CHECK(queue.try_push(i));
    }
    CHECK(!queue.try_push(0));

    value_type value = 0;
    for (value_type i = 0; i < kQueueCapacity; ++i) {
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, i);
    }
    CHECK(!queue.try_pop(value));
    CHECK(queue.empty());
}

MPMC_TEST(spsc_threaded_ordering) {
    run_spsc_ordering<
        mpmc::spsc_queue<value_type, kQueueCapacity, index_type>>();
}

MPMC_TEST(spmc_threaded_unique_items) {
    mpmc::spmc_queue<value_type, kQueueCapacity, index_type> queue;
    std::atomic<bool> go{false};
    std::atomic<std::uint32_t> received{0};
    std::vector<std::atomic<unsigned char>> seen(kItems);
    for (auto& entry : seen) {
        entry.store(0, std::memory_order_relaxed);
    }
    std::atomic<bool> duplicate{false};

    std::thread producer([&] {
        wait_until(go);
        for (std::uint32_t i = 0; i < kItems; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::vector<std::thread> consumers;
    consumers.reserve(4);
    for (int c = 0; c < 4; ++c) {
        consumers.emplace_back([&] {
            wait_until(go);
            while (received.load(std::memory_order_acquire) < kItems) {
                value_type value = 0;
                if (!queue.try_pop(value)) {
                    std::this_thread::yield();
                    continue;
                }
                if (value >= kItems ||
                    seen[value].exchange(1, std::memory_order_relaxed) != 0) {
                    duplicate.store(true, std::memory_order_relaxed);
                }
                received.fetch_add(1, std::memory_order_release);
            }
        });
    }

    go.store(true, std::memory_order_release);
    producer.join();
    for (auto& consumer : consumers) {
        consumer.join();
    }

    CHECK(!duplicate.load(std::memory_order_relaxed));
    CHECK_EQ(received.load(std::memory_order_relaxed), kItems);
    for (const auto& entry : seen) {
        CHECK_EQ(entry.load(std::memory_order_relaxed), 1);
    }
}

MPMC_TEST(mpsc_threaded_unique_items) {
    mpmc::mpsc_queue<value_type, kQueueCapacity, index_type> queue;
    std::atomic<bool> go{false};
    constexpr std::uint32_t producers = 4;
    constexpr std::uint32_t per_producer = kItems / producers;
    std::vector<unsigned char> seen(static_cast<std::size_t>(producers) *
                                    per_producer);

    std::vector<std::thread> producer_threads;
    producer_threads.reserve(producers);
    for (std::uint32_t p = 0; p < producers; ++p) {
        producer_threads.emplace_back([&, p] {
            wait_until(go);
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                const value_type value = p * per_producer + i;
                while (!queue.try_push(value)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::thread consumer([&] {
        wait_until(go);
        std::uint32_t count = 0;
        while (count < producers * per_producer) {
            value_type value = 0;
            if (!queue.try_pop(value)) {
                std::this_thread::yield();
                continue;
            }
            CHECK(value < seen.size());
            CHECK_EQ(seen[value], 0);
            seen[value] = 1;
            ++count;
        }
    });

    go.store(true, std::memory_order_release);
    for (auto& producer : producer_threads) {
        producer.join();
    }
    consumer.join();

    for (unsigned char entry : seen) {
        CHECK_EQ(entry, 1);
    }
}

MPMC_TEST(mpsc_observer_queries_are_concurrency_safe) {
    mpmc::mpsc_queue<value_type, kQueueCapacity, index_type> queue;
    std::atomic<bool> go{false};

    std::thread producer([&] {
        wait_until(go);
        for (std::uint32_t i = 0; i < kItems; ++i) {
            while (!queue.try_push(i)) {
                std::this_thread::yield();
            }
            if ((i & 1023U) == 0U) {
                (void)queue.empty();
                (void)queue.size();
            }
        }
    });

    std::thread consumer([&] {
        wait_until(go);
        std::uint32_t count = 0;
        while (count < kItems) {
            value_type value = 0;
            if (queue.try_pop(value)) {
                CHECK_EQ(value, count);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    CHECK(queue.empty());
}

MPMC_TEST(mpmc_high_contention_progress) {
    constexpr std::uint32_t producers = 4;
    constexpr std::uint32_t consumers = 4;
    constexpr std::uint32_t per_producer = 250'000;
    mpmc::mpmc_queue<value_type, kQueueCapacity, index_type> queue;
    std::atomic<bool> go{false};
    std::atomic<std::uint32_t> produced{0};
    std::atomic<std::uint32_t> consumed{0};
    std::atomic<std::uint64_t> sum{0};
    std::atomic<bool> invalid{false};

    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);
    for (std::uint32_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            wait_until(go);
            const std::uint32_t base = p * per_producer;
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                while (!queue.try_push(base + i)) {
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::uint32_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&] {
            wait_until(go);
            for (;;) {
                value_type value = 0;
                if (queue.try_pop(value)) {
                    if (value >= producers * per_producer) {
                        invalid.store(true, std::memory_order_relaxed);
                        return;
                    }
                    consumed.fetch_add(1, std::memory_order_release);
                    sum.fetch_add(value, std::memory_order_relaxed);
                    continue;
                }
                if (produced.load(std::memory_order_acquire) ==
                    producers * per_producer) {
                    while (queue.try_pop(value)) {
                        consumed.fetch_add(1, std::memory_order_release);
                        sum.fetch_add(value, std::memory_order_relaxed);
                    }
                    return;
                }
                std::this_thread::yield();
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK_EQ(consumed.load(std::memory_order_relaxed),
             producers * per_producer);
    const std::uint64_t n =
        static_cast<std::uint64_t>(producers) * per_producer;
    CHECK_EQ(sum.load(std::memory_order_relaxed), (n - 1) * n / 2);
    CHECK(!invalid.load(std::memory_order_relaxed));

    REQUIRE(queue.try_push(10));
    REQUIRE(queue.try_push(20));
    REQUIRE(queue.try_push(30));
    REQUIRE(queue.try_replace_last(99));
    value_type value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, value_type{10});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, value_type{20});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, value_type{99});
}

template<class T>
concept spsc_type_available =
    requires { typename mpmc::spsc_queue<T, 8, index_type>; };

template<class Q>
concept static_spsc_factory =
    requires(std::error_code& error) { Q::create(error); };

template<class Q>
concept runtime_spsc_factory =
    requires(std::size_t capacity, std::error_code& error) {
        Q::create(capacity, error);
    };

MPMC_TEST(spsc_constraints_and_factories) {
    static_assert(!spsc_type_available<const std::uint32_t>);
    static_assert(!spsc_type_available<volatile std::uint32_t>);

    using static_queue = mpmc::spsc_queue<value_type, 8, index_type>;
    using runtime_queue = mpmc::spsc_queue<value_type, 0, index_type>;
    static_assert(static_spsc_factory<static_queue>);
    static_assert(!runtime_spsc_factory<static_queue>);
    static_assert(runtime_spsc_factory<runtime_queue>);
    static_assert(!static_spsc_factory<runtime_queue>);
}

template<class Q>
concept has_keep_n =
    requires(Q& queue, value_type value) { queue.push_keep_n(value); };

MPMC_TEST(single_consumer_keeps_keep_n_unavailable) {
    using spsc = mpmc::spsc_queue<value_type, kQueueCapacity, index_type>;
    using spmc = mpmc::spmc_queue<value_type, kQueueCapacity, index_type>;
    using mpsc = mpmc::mpsc_queue<value_type, kQueueCapacity, index_type>;

    static_assert(!has_keep_n<spsc>);
    static_assert(!has_keep_n<mpsc>);
    static_assert(has_keep_n<spmc>);
}

} // namespace
