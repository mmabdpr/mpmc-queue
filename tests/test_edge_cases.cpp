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
#include "mpmc/mpmc.hpp"

#include "test_cell_config.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using cell_value = mpmc_test::wide_value;
using cell_index = mpmc_test::wide_index;

struct oversized_payload {
    std::uint64_t first;
    std::uint64_t second;
    std::uint64_t third;
};

struct unapproved_shared_payload {
    std::uint32_t value;
};

static_assert(mpmc::supported_index<std::uint32_t>);
static_assert(mpmc::supported_index<std::uint64_t>);
static_assert(!mpmc::supported_index<int>);
static_assert(!mpmc::supported_index<bool>);
static_assert(!mpmc::supported_index<const std::uint32_t>);
static_assert(!mpmc::supported_index<volatile std::uint32_t>);
static_assert(!mpmc::supported_index<const bool>);
static_assert(!mpmc::supported_index<void>);

static_assert(mpmc::queue_payload<std::uint32_t>);
static_assert(mpmc::queue_payload<oversized_payload>);
static_assert(!mpmc::queue_payload<const std::uint32_t>);
static_assert(!mpmc::queue_payload<void>);
static_assert(mpmc::queue_payload_for<std::uint32_t, std::uint32_t>);
static_assert(!mpmc::queue_payload_for<oversized_payload, std::uint32_t>);
static_assert(!mpmc::queue_payload_for<std::uint32_t, int>);

static_assert(mpmc::valid_static_capacity<0, std::uint32_t>);
static_assert(mpmc::valid_static_capacity<8, std::uint32_t>);
static_assert(!mpmc::valid_static_capacity<3, std::uint32_t>);
static_assert(!mpmc::valid_static_capacity<0, void>);

static_assert(mpmc::unique_ptr_payload<int>);
static_assert(!mpmc::unique_ptr_payload<int[]>);
static_assert(!mpmc::unique_ptr_payload<void>);
#if !defined(_WIN32)
static_assert(mpmc::shared_memory_payload<std::uint32_t>);
static_assert(!mpmc::shared_memory_payload<unapproved_shared_payload>);
#endif

template<class T, std::size_t Capacity, class Index>
concept queue_available =
    requires { typename mpmc::mpmc_queue<T, Capacity, Index>; };

template<class T, std::size_t Capacity, class Index>
concept unique_queue_available =
    requires { typename mpmc::unique_ptr_queue<T, Capacity, Index>; };

static_assert(queue_available<std::uint32_t, 8, std::uint32_t>);
static_assert(!queue_available<oversized_payload, 8, std::uint32_t>);
static_assert(!queue_available<std::uint32_t, 3, std::uint32_t>);
static_assert(!queue_available<std::uint32_t, 8, int>);
static_assert(!queue_available<std::uint32_t, 0, const std::uint32_t>);
static_assert(!queue_available<std::uint32_t, 0, const bool>);
static_assert(unique_queue_available<int, 8, std::uint32_t>);
static_assert(!unique_queue_available<int[], 8, std::uint32_t>);

template<bool LazyPush, bool LazyPop> void lazy_roundtrip() {
    mpmc::mpmc_queue<cell_value, 8, cell_index, LazyPush, LazyPop> queue;

    for (cell_value i = 0; i < 8; ++i) {
        REQUIRE(queue.try_push(i));
    }
    CHECK(!queue.try_push(99));

    for (cell_value i = 0; i < 8; ++i) {
        cell_value value = 0;
        cell_value sequence = 100;
        REQUIRE(queue.try_pop(value, sequence));
        CHECK_EQ(value, i);
        CHECK_EQ(sequence, i);
    }

    CHECK(queue.empty());
    CHECK_EQ(queue.size(), std::size_t{0});

    cell_value value = 0;
    CHECK(!queue.try_pop(value));

    // The queue must keep working after the empty checks.
    CHECK(queue.try_push(1234));
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{1234});
}

template<bool LazyPush, bool LazyPop> void lazy_multithreaded() {
    constexpr std::uint32_t producers = 2;
    constexpr std::uint32_t consumers = 2;
    constexpr std::uint32_t per_producer = 4000;
    constexpr std::uint32_t total = producers * per_producer;

    mpmc::mpmc_queue<cell_value, 32, cell_index, LazyPush, LazyPop> queue;
    std::atomic<std::uint32_t> produced{0};
    std::atomic<std::uint32_t> consumed{0};
    std::atomic<bool> failed{false};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);

    auto producer = [&](std::uint32_t id) {
        try {
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                const cell_value value =
                    (static_cast<cell_value>(id) << 24U) | i;
                while (!queue.try_push(value)) {
                    if (failed.load(std::memory_order_acquire) ||
                        std::chrono::steady_clock::now() > deadline) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_release);
            }
        } catch (...) {
            failed.store(true, std::memory_order_release);
        }
    };

    auto consumer = [&]() {
        try {
            for (;;) {
                cell_value value = 0;
                if (queue.try_pop(value)) {
                    (void)value;
                    const std::uint32_t count =
                        consumed.fetch_add(1, std::memory_order_acq_rel) + 1;
                    if (count == total) {
                        return;
                    }
                } else if (consumed.load(std::memory_order_acquire) >= total) {
                    return;
                } else {
                    if (failed.load(std::memory_order_acquire) ||
                        std::chrono::steady_clock::now() > deadline) {
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
    threads.reserve(producers + consumers);
    for (std::uint32_t i = 0; i < producers; ++i) {
        threads.emplace_back(producer, i);
    }
    for (std::uint32_t i = 0; i < consumers; ++i) {
        threads.emplace_back(consumer);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK_EQ(consumed.load(std::memory_order_acquire), total);
    CHECK(queue.empty());
}

template<class Index> void narrow_index_roundtrip(std::size_t iterations) {
    mpmc::mpmc_queue<std::uint16_t, 8, Index> queue;
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::uint16_t value = static_cast<std::uint16_t>(i & 0xffffU);
        REQUIRE(queue.try_push(value));
        std::uint16_t popped = 0;
        REQUIRE(queue.try_pop(popped));
        CHECK_EQ(popped, value);
    }
    CHECK(queue.empty());
}

} // namespace

MPMC_TEST(lazy_mode_matrix) {
    lazy_roundtrip<false, false>();
    lazy_roundtrip<true, false>();
    lazy_roundtrip<false, true>();
    lazy_roundtrip<true, true>();
}

MPMC_TEST(lazy_push_size_snapshot) {
    mpmc::mpmc_queue<cell_value, 8, cell_index, true, false> queue;
    REQUIRE(queue.try_push(7));
    cell_value value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{7});
    CHECK(queue.empty());
    // The lazy write index still points at the consumed position, so the
    // read index is one ahead.  size() must not turn that into capacity.
    CHECK_EQ(queue.size(), std::size_t{0});
    REQUIRE(queue.try_push(8));
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{8});
}

MPMC_TEST(lazy_mode_matrix_threaded) {
    lazy_multithreaded<false, false>();
    lazy_multithreaded<true, false>();
    lazy_multithreaded<false, true>();
    lazy_multithreaded<true, true>();
}

MPMC_TEST(narrow_index_types) {
    narrow_index_roundtrip<std::uint8_t>(60);
    narrow_index_roundtrip<std::uint16_t>(10000);
    narrow_index_roundtrip<std::uint32_t>(10000);
#if MPMC_TEST_HAS_16_BYTE_CELL
    narrow_index_roundtrip<std::uint64_t>(10000);
#endif

    // Exercise both the slot-index and the marker arithmetic close to the
    // 8-bit marker horizon without crossing it.
    mpmc::mpmc_queue<std::uint8_t, 2, std::uint8_t> queue;
    for (std::uint8_t i = 0; i < 120; ++i) {
        REQUIRE(queue.try_push(i));
        std::uint8_t value = 0;
        std::uint8_t sequence = 0;
        REQUIRE(queue.try_pop(value, sequence));
        CHECK_EQ(value, i);
        CHECK_EQ(sequence, i);
    }
    CHECK(queue.empty());
}

MPMC_TEST(heap_queue_factory_for_large_capacity) {
    using queue_type = mpmc::mpmc_queue<std::uint32_t, 1024, std::uint32_t>;
    static_assert(queue_type::storage_bytes == std::size_t{1024} * 128);
    std::error_code error;
    auto queue = mpmc::make_queue<queue_type>(error);
    REQUIRE(!error);
    REQUIRE(queue != nullptr);
    CHECK_EQ(queue->capacity(), std::size_t{1024});
    REQUIRE(queue->try_push(123));
    std::uint32_t value = 0;
    REQUIRE(queue->try_pop(value));
    CHECK_EQ(value, std::uint32_t{123});

    auto runtime_queue =
        mpmc::mpmc_queue<std::uint32_t, 0, std::uint32_t>::create(64, error);
    REQUIRE(!error);
    REQUIRE(runtime_queue != nullptr);
    CHECK_EQ(runtime_queue->capacity(), std::size_t{64});
}

MPMC_TEST(sequence_exhaustion_fails_stop) {
    using queue_type = mpmc::mpmc_queue<std::uint8_t, 2, std::uint8_t>;
    queue_type queue;

    // The publication limit reserves capacity positions so that the final
    // empty marker still fits in the marker word.
    CHECK_EQ(queue.publication_limit(), std::uint8_t{126});

    for (std::uint8_t i = 0; i < 126; ++i) {
        REQUIRE(queue.try_push(i));
        std::uint8_t value = 0;
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, i);
    }

    CHECK(queue.sequence_exhausted());
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{0});
    CHECK(!queue.try_push(1));
    CHECK(!queue.push_keep_n(1));
    std::uint8_t value = 0;
    CHECK(!queue.try_pop(value));
    CHECK(queue.empty());

    // clear() is not concurrent-safe, but it starts a new generation at the
    // same capacity.
    queue.clear();
    CHECK(!queue.sequence_exhausted());
    CHECK(queue.try_push(42));
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, std::uint8_t{42});
}

MPMC_TEST(push_keep_n_preserves_backlog_at_exhaustion) {
    mpmc::mpmc_queue<std::uint8_t, 2, std::uint8_t> queue;
    for (std::uint8_t i = 0; i < 124; ++i) {
        REQUIRE(queue.try_push(i));
        std::uint8_t value = 0;
        REQUIRE(queue.try_pop(value));
    }
    REQUIRE(queue.try_push(124));
    REQUIRE(queue.try_push(125));
    CHECK(queue.sequence_exhausted());
    CHECK(!queue.push_keep_n(7));

    std::uint8_t value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, std::uint8_t{124});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, std::uint8_t{125});
}

MPMC_TEST(sequence_exhaustion_lazy_modes) {
    using queue_type =
        mpmc::mpmc_queue<std::uint8_t, 2, std::uint8_t, true, true>;
    queue_type queue;
    for (std::uint8_t i = 0; i < 126; ++i) {
        REQUIRE(queue.try_push(i));
        std::uint8_t value = 0;
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, i);
    }
    // Lazy modes may leave both indices one step behind the physical slot
    // marker until the next attempt helps advance them.  Diagnostics are
    // allowed to lag, so exhaustion and the remaining count are not final
    // until a helping operation.
    CHECK(!queue.sequence_exhausted());
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{1});

    // The failed publication helps advance the write index to the horizon.
    CHECK(!queue.try_push(1));
    CHECK(queue.sequence_exhausted());
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{0});
    CHECK(queue.empty());
}

MPMC_TEST(static_members_match_template_arguments) {
    using queue_type = mpmc::bounded_queue<cell_value, 8, cell_index>;
    static_assert(std::is_same_v<queue_type,
                                 mpmc::mpmc_queue<cell_value, 8, cell_index>>);
    static_assert(queue_type::is_always_lock_free);
    static_assert(queue_type::entry_size ==
                  sizeof(cell_value) + sizeof(cell_index));

    queue_type queue;
    CHECK_EQ(queue.capacity(), std::size_t{8});
    CHECK(queue_type::is_lock_free());
    CHECK_EQ(mpmc::version_major, 1);
#if defined(_WIN32)
    CHECK(!mpmc::shared_memory_available);
#else
    CHECK(mpmc::shared_memory_available);
#endif
}

MPMC_TEST(lazy_replace_last) {
    mpmc::mpmc_queue<cell_value, 4, cell_index, true, false> queue;
    REQUIRE(queue.try_push(1));
    cell_value sequence = 99;
    CHECK(queue.try_replace_last(9, &sequence));
    CHECK_EQ(sequence, cell_value{0});
    cell_value value = 0;
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{9});

    REQUIRE(queue.try_push(2));
    CHECK(queue.try_push_or_replace(10, &sequence));
    CHECK_EQ(sequence, cell_value{1});
    REQUIRE(queue.try_pop(value));
    CHECK_EQ(value, cell_value{10});
}

MPMC_TEST(full_queue_after_reuse) {
    mpmc::mpmc_queue<cell_value, 2, cell_index> queue;
    for (cell_value cycle = 0; cycle < 100; ++cycle) {
        REQUIRE(queue.try_push(cycle * 2));
        REQUIRE(queue.try_push(cycle * 2 + 1));
        CHECK(!queue.try_push(999));

        cell_value value = 0;
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, cycle * 2);
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, cycle * 2 + 1);
        CHECK(queue.empty());
    }
}

MPMC_TEST(spsc_runtime_capacity_and_error_paths) {
    std::error_code error =
        std::make_error_code(std::errc::operation_not_permitted);
    auto queue = mpmc::spsc_queue<std::uint64_t>::create(64, error);
    REQUIRE(queue != nullptr);
    CHECK(!error);
    CHECK_EQ(queue->capacity(), 64u);
    for (std::uint64_t i = 0; i < 64; ++i) {
        REQUIRE(queue->try_push(i));
    }
    CHECK(!queue->try_push(64));
    for (std::uint64_t i = 0; i < 64; ++i) {
        std::uint64_t value = 0;
        REQUIRE(queue->try_pop(value));
        CHECK_EQ(value, i);
    }
    std::uint64_t empty_value = 0;
    CHECK(!queue->try_pop(empty_value));
    CHECK(queue->empty());

    error.clear();
    auto bad = mpmc::spsc_queue<std::uint64_t>::create(3, error);
    CHECK(bad == nullptr);
    CHECK(error == mpmc::errc::invalid_capacity);

    error.clear();
    auto static_queue = mpmc::spsc_queue<std::uint64_t, 4>::create(error);
    REQUIRE(static_queue != nullptr);
    CHECK(!error);
    CHECK_EQ(static_queue->capacity(), 4u);
}

MPMC_TEST(spsc_sequence_horizon) {
    mpmc::spsc_queue<std::uint8_t, 2, std::uint8_t> queue;
    CHECK_EQ(queue.publication_limit(), 126);
    for (std::uint8_t i = 0; i < 126; ++i) {
        REQUIRE(queue.try_push(i));
        std::uint8_t value = 0;
        REQUIRE(queue.try_pop(value));
        CHECK_EQ(value, i);
    }
    CHECK(queue.sequence_exhausted());
    CHECK_EQ(queue.publications_remaining(), 0);
    CHECK(!queue.try_push(0));
    queue.clear();
    CHECK(!queue.sequence_exhausted());
    CHECK(queue.try_push(7));
}
