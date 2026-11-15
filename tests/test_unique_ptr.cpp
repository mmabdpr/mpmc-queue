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

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

struct tracked {
    static inline std::atomic<int> alive{0};

    explicit tracked(int value_in) : value(value_in) {
        alive.fetch_add(1, std::memory_order_relaxed);
    }
    ~tracked() {
        alive.fetch_sub(1, std::memory_order_relaxed);
    }

    tracked(const tracked&) = delete;
    tracked& operator=(const tracked&) = delete;

    int value;
};

using checked_unique_ptr_queue = mpmc::unique_ptr_queue<tracked, 4>;
static_assert(std::is_same_v<
              decltype(std::declval<checked_unique_ptr_queue&>().try_pop()),
              std::optional<std::unique_ptr<tracked>>>);
static_assert(noexcept(std::declval<checked_unique_ptr_queue&>().try_pop()));
static_assert(noexcept(std::declval<checked_unique_ptr_queue&>().try_pop(
    std::declval<std::unique_ptr<tracked>&>())));
static_assert(noexcept(
    std::declval<const checked_unique_ptr_queue&>().sequence_exhausted()));
static_assert(noexcept(
    std::declval<const checked_unique_ptr_queue&>().publications_remaining()));
static_assert(noexcept(
    std::declval<const checked_unique_ptr_queue&>().publication_limit()));
static_assert(checked_unique_ptr_queue::marker_horizon == std::uint64_t{1}
                                                              << 63U);

} // namespace

MPMC_TEST(unique_ptr_ownership_transfer) {
    tracked::alive.store(0, std::memory_order_relaxed);
    {
        mpmc::unique_ptr_queue<tracked, 4> queue;

        auto first = std::make_unique<tracked>(1);
        tracked* first_raw = first.get();
        std::uint64_t sequence = 99;
        REQUIRE(queue.try_push(std::move(first), sequence));
        CHECK(!first);
        CHECK_EQ(sequence, std::uint64_t{0});
        CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 1);

        REQUIRE(queue.try_push(std::make_unique<tracked>(2)));
        REQUIRE(queue.try_push(std::make_unique<tracked>(3)));
        REQUIRE(queue.try_push(std::make_unique<tracked>(4)));

        auto rejected = std::make_unique<tracked>(5);
        CHECK(!queue.try_push(std::move(rejected)));
        REQUIRE(rejected != nullptr);
        // Ownership is retained when the rvalue-reference push fails.
        CHECK_EQ(rejected->value, 5); // NOLINT(bugprone-use-after-move)
        CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 5);

        std::unique_ptr<tracked> out;
        REQUIRE(queue.try_pop(out));
        REQUIRE(out != nullptr);
        CHECK_EQ(out->value, 1);
        CHECK(out.get() == first_raw);
        CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 5);

        // Popping into an owning pointer replaces its old object.
        REQUIRE(queue.try_pop(out));
        CHECK_EQ(out->value, 2);
        CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 4);

        const std::optional<std::unique_ptr<tracked>> optional =
            queue.try_pop();
        REQUIRE(optional.has_value());
        CHECK_EQ((*optional)->value, 3);
        CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 4);

        // The fourth object is still owned by the queue and must be deleted
        // when the queue is destroyed.
    }
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 0);
}

MPMC_TEST(unique_ptr_runtime_factory) {
    tracked::alive.store(0, std::memory_order_relaxed);
    std::error_code error;
    auto owner = mpmc::unique_ptr_queue<tracked>::create(4, error);
    REQUIRE(!error);
    REQUIRE(owner);

    REQUIRE(owner->try_push(std::make_unique<tracked>(7)));
    std::unique_ptr<tracked> item;
    REQUIRE(owner->try_pop(item));
    REQUIRE(item);
    CHECK_EQ(item->value, 7);
    item.reset();
    owner.reset();
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 0);
}

MPMC_TEST(unique_ptr_clear) {
    tracked::alive.store(0, std::memory_order_relaxed);
    mpmc::unique_ptr_queue<tracked, 8> queue;
    for (int i = 0; i < 6; ++i) {
        REQUIRE(queue.try_push(std::make_unique<tracked>(i)));
    }
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 6);

    queue.clear();
    CHECK(queue.empty());
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 0);

    REQUIRE(queue.try_push(std::make_unique<tracked>(7)));
    std::unique_ptr<tracked> out;
    REQUIRE(queue.try_pop(out));
    CHECK_EQ(out->value, 7);
    out.reset();
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 0);
}

MPMC_TEST(unique_ptr_exhaustion_diagnostics_and_clear) {
    tracked::alive.store(0, std::memory_order_relaxed);
    using queue_type = mpmc::unique_ptr_queue<tracked, 2, std::uint8_t>;
    static_assert(queue_type::marker_horizon == std::uint8_t{128});

    queue_type queue;
    CHECK_EQ(queue.publication_limit(), std::uint8_t{126});
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{126});
    CHECK(!queue.sequence_exhausted());

    for (std::uint8_t i = 0; i < 124; ++i) {
        REQUIRE(queue.try_push(std::make_unique<tracked>(i)));
        std::unique_ptr<tracked> out;
        REQUIRE(queue.try_pop(out));
        CHECK_EQ(out->value, static_cast<int>(i));
    }
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{2});

    // Fill the last two publication positions and stop there.  The queue is
    // exhausted but still drainable, and owns both pointers.
    REQUIRE(queue.try_push(std::make_unique<tracked>(124)));
    REQUIRE(queue.try_push(std::make_unique<tracked>(125)));
    CHECK(queue.sequence_exhausted());
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{0});
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 2);

    // A rejected push retains caller ownership, even at the horizon.
    auto rejected = std::make_unique<tracked>(7);
    tracked* rejected_raw = rejected.get();
    CHECK(!queue.try_push(std::move(rejected)));
    REQUIRE(rejected != nullptr);
    CHECK(rejected.get() == rejected_raw);
    // Ownership is retained when the rvalue-reference push fails.
    CHECK_EQ(rejected->value, 7); // NOLINT(bugprone-use-after-move)
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 3);

    // clear() deletes the retained backlog and starts a new generation.
    queue.clear();
    CHECK(queue.empty());
    CHECK_EQ(queue.size(), std::size_t{0});
    CHECK(!queue.sequence_exhausted());
    CHECK_EQ(queue.publications_remaining(), std::uint8_t{126});
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 1);

    REQUIRE(queue.try_push(std::move(rejected)));
    CHECK(!rejected);
    std::unique_ptr<tracked> out;
    REQUIRE(queue.try_pop(out));
    CHECK_EQ(out->value, 7);
    out.reset();
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 0);
}

MPMC_TEST(unique_ptr_null_push) {
    mpmc::unique_ptr_queue<tracked, 2> queue;
    std::unique_ptr<tracked> empty;
    CHECK(!queue.try_push(std::move(empty)));
    CHECK(!queue.try_push(nullptr));
    CHECK(queue.empty());
}

MPMC_TEST(unique_ptr_threaded) {
    tracked::alive.store(0, std::memory_order_relaxed);
    constexpr int producers = 2;
    constexpr int consumers = 2;
    constexpr int per_producer = 5000;
    constexpr int total = producers * per_producer;

    mpmc::unique_ptr_queue<tracked, 64> queue;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::atomic<bool> failed{false};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers) +
                    static_cast<std::size_t>(consumers));
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < per_producer; ++i) {
                if (failed.load(std::memory_order_acquire)) {
                    return;
                }
                auto item = std::make_unique<tracked>(p * per_producer + i);
                while (!queue.try_push(std::move(item))) {
                    if (!item || failed.load(std::memory_order_acquire) ||
                        std::chrono::steady_clock::now() > deadline) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_release);
            }
        });
    }
    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&] {
            for (;;) {
                std::unique_ptr<tracked> item;
                if (queue.try_pop(item)) {
                    const int count =
                        consumed.fetch_add(1, std::memory_order_acq_rel) + 1;
                    item.reset();
                    if (count == total) {
                        return;
                    }
                } else if (produced.load(std::memory_order_acquire) == total &&
                           queue.empty()) {
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
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK_EQ(consumed.load(std::memory_order_acquire), total);
    CHECK(queue.empty());
    queue.clear();
    CHECK_EQ(tracked::alive.load(std::memory_order_relaxed), 0);
}
