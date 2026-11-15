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
// Concurrency matrix coverage:
//   * 8-byte and (where lock-free) 16-byte cells
//   * all eager/lazy push/pop combinations
//   * capacities 1, 8 and 64
//   * concurrent replace/pop_if interaction with payload-integrity checks
//   * a deterministic pop_if accepted-candidate/replacement race
//   * narrow-index concurrent publication exhaustion
//
// Every wait is bounded by a deadline, so a regression fails the test instead
// of hanging CTest.

#include "mpmc/mpmc.hpp"

#include "test_cell_config.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

[[nodiscard]] bool expired(clock_type::time_point deadline) noexcept {
    return clock_type::now() > deadline;
}

template<class Value> struct observation {
    std::uint64_t sequence;
    Value value;
};

// ------------------------------------------------------------- payloads
//
// A replacement-aware run publishes exactly one of two things at each
// position: the original producer/item pair or the designated replacement
// marker.  Verification compares the whole payload, so a torn atomic-cell
// snapshot (half from the original publication, half from the replacement)
// matches neither and is reported.

struct encoded_word_payload {
    using value_type = std::uint32_t;

    static constexpr value_type replacement_marker = 0x00fffffeU;

    [[nodiscard]] static value_type make(std::uint32_t producer,
                                         std::uint32_t item) noexcept {
        return static_cast<value_type>((producer << 24U) | item);
    }

    [[nodiscard]] static value_type replacement() noexcept {
        return replacement_marker;
    }

    [[nodiscard]] static bool is_well_formed(const value_type&) noexcept {
        // Every 32-bit pattern is a possible publication; the exact-match
        // checks below are what detect corruption.
        return true;
    }

    [[nodiscard]] static bool is_replacement(const value_type& value) noexcept {
        return value == replacement_marker;
    }

    [[nodiscard]] static bool is_intact(const value_type& value,
                                        const value_type& original) noexcept {
        return value == original || value == replacement_marker;
    }

    [[nodiscard]] static bool
    predicate_accepts(const value_type& value) noexcept {
        return !is_replacement(value);
    }

    [[nodiscard]] static bool decode(const value_type& value,
                                     std::uint32_t& producer,
                                     std::uint32_t& item) noexcept {
        producer = value >> 24U;
        item = value & 0x00ffffffU;
        return true;
    }
};

// Twelve payload bytes plus a 32-bit index is exactly one 16-byte atomic cell.
// The checksum is derived from the other two fields, so a snapshot that mixes
// fields from two publications fails validation even before it is compared
// with the expected original.
struct checksummed_payload {
    std::uint32_t producer;
    std::uint32_t item;
    std::uint32_t checksum;

    friend bool operator==(const checksummed_payload&,
                           const checksummed_payload&) = default;
};
static_assert(sizeof(checksummed_payload) == 12);
static_assert(sizeof(checksummed_payload) + sizeof(std::uint32_t) == 16);

[[nodiscard]] constexpr std::uint32_t
payload_checksum(std::uint32_t producer, std::uint32_t item) noexcept {
    std::uint32_t mixed = producer * 0x9e3779b9U + item * 0x85ebca6bU;
    mixed ^= mixed >> 16U;
    mixed *= 0xc2b2ae35U;
    mixed ^= mixed >> 13U;
    return mixed;
}

struct checksummed_cell_payload {
    using value_type = checksummed_payload;

    static constexpr std::uint32_t replacement_producer = 0xffffffffU;

    [[nodiscard]] static value_type make(std::uint32_t producer,
                                         std::uint32_t item) noexcept {
        return value_type{producer, item, payload_checksum(producer, item)};
    }

    [[nodiscard]] static value_type replacement() noexcept {
        return make(replacement_producer, 0);
    }

    [[nodiscard]] static bool is_well_formed(const value_type& value) noexcept {
        return value.checksum == payload_checksum(value.producer, value.item);
    }

    [[nodiscard]] static bool is_replacement(const value_type& value) noexcept {
        return value == replacement();
    }

    [[nodiscard]] static bool is_intact(const value_type& value,
                                        const value_type& original) noexcept {
        return is_well_formed(value) &&
               (value == original || is_replacement(value));
    }

    [[nodiscard]] static bool
    predicate_accepts(const value_type& value) noexcept {
        return is_well_formed(value) && !is_replacement(value);
    }

    [[nodiscard]] static bool decode(const value_type& value,
                                     std::uint32_t& producer,
                                     std::uint32_t& item) noexcept {
        producer = value.producer;
        item = value.item;
        return is_well_formed(value);
    }
};

template<class Value, class Index, std::size_t Capacity, bool LazyPush,
         bool LazyPop>
void lossless_matrix(std::uint32_t producer_count, std::uint32_t consumer_count,
                     std::uint32_t per_producer) {
    using queue_type =
        mpmc::mpmc_queue<Value, Capacity, Index, LazyPush, LazyPop>;

    const std::uint64_t total =
        static_cast<std::uint64_t>(producer_count) * per_producer;
    queue_type queue;

    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<bool> failed{false};
    std::vector<std::vector<observation<Value>>> observations(consumer_count);
    const auto deadline = clock_type::now() + std::chrono::seconds(30);

    auto producer = [&](std::uint32_t id) {
        for (std::uint32_t i = 0; i < per_producer; ++i) {
            if (failed.load(std::memory_order_acquire)) {
                return;
            }
            const Value value =
                static_cast<Value>((static_cast<std::uint32_t>(id) << 24U) | i);
            Index sequence{};
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
            produced.fetch_add(1, std::memory_order_release);
        }
    };

    auto consumer = [&](std::uint32_t id) {
        std::vector<observation<Value>>& local = observations[id];
        local.reserve(per_producer * producer_count / consumer_count + 16);
        for (;;) {
            Value value{};
            Index sequence{};
            if (queue.try_pop(value, sequence)) {
                local.push_back(observation<Value>{
                    static_cast<std::uint64_t>(sequence), value});
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

    std::vector<observation<Value>> all;
    all.reserve(static_cast<std::size_t>(total));
    for (const auto& local : observations) {
        all.insert(all.end(), local.begin(), local.end());
    }
    std::sort(all.begin(), all.end(),
              [](const observation<Value>& a, const observation<Value>& b) {
                  return a.sequence < b.sequence;
              });
    REQUIRE_EQ(all.size(), static_cast<std::size_t>(total));

    std::vector<bool> seen(static_cast<std::size_t>(total), false);
    std::vector<std::int64_t> last_item(producer_count, -1);
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK_EQ(all[i].sequence, static_cast<std::uint64_t>(i));
        if (all[i].sequence < total) {
            CHECK(!seen[static_cast<std::size_t>(all[i].sequence)]);
            seen[static_cast<std::size_t>(all[i].sequence)] = true;
        }
        const std::uint32_t producer_id =
            static_cast<std::uint32_t>(all[i].value >> 24U);
        const std::uint32_t item =
            static_cast<std::uint32_t>(all[i].value & 0x00ffffffU);
        REQUIRE(producer_id < producer_count);
        CHECK(item < per_producer);
        CHECK(static_cast<std::int64_t>(item) > last_item[producer_id]);
        last_item[producer_id] = static_cast<std::int64_t>(item);
    }
    for (bool flag : seen) {
        CHECK(flag);
    }
}

template<class Payload, class Index, std::size_t Capacity, bool LazyPush,
         bool LazyPop>
void replace_pop_if_race(std::uint32_t producer_count,
                         std::uint32_t consumer_count,
                         std::uint32_t per_producer) {
    using value_type = typename Payload::value_type;
    using queue_type =
        mpmc::mpmc_queue<value_type, Capacity, Index, LazyPush, LazyPop>;

    const std::uint64_t total =
        static_cast<std::uint64_t>(producer_count) * per_producer;
    queue_type queue;

    // The exact value each publication position should contain before any
    // replacement.  Producers write these slots after publishing; readers only
    // touch the vector after every thread has joined.
    std::vector<value_type> originals(static_cast<std::size_t>(total));

    std::atomic<std::uint64_t> produced{0};
    std::atomic<std::uint64_t> consumed{0};
    std::atomic<std::uint64_t> replacements{0};
    std::atomic<bool> failed{false};
    std::atomic<bool> stop_replacer{false};
    std::atomic<bool> replacement_seen{false};
    std::vector<std::atomic<std::uint8_t>> seen(total);
    for (auto& count : seen) {
        count.store(0, std::memory_order_relaxed);
    }
    std::vector<std::vector<observation<value_type>>> observations(
        consumer_count);
    const auto deadline = clock_type::now() + std::chrono::seconds(30);

    auto note_failure = [&] { failed.store(true, std::memory_order_release); };

    auto producer = [&](std::uint32_t id) {
        for (std::uint32_t i = 0; i < per_producer; ++i) {
            if (failed.load(std::memory_order_acquire)) {
                return;
            }
            const value_type value = Payload::make(id, i);
            Index sequence{};
            while (!queue.try_push(value, sequence)) {
                if (failed.load(std::memory_order_acquire) ||
                    expired(deadline)) {
                    note_failure();
                    return;
                }
                std::this_thread::yield();
            }
            if (static_cast<std::uint64_t>(sequence) >= total) {
                note_failure();
                return;
            }
            originals[static_cast<std::size_t>(sequence)] = value;
            produced.fetch_add(1, std::memory_order_release);
        }
    };

    auto replacer = [&] {
        const value_type marker = Payload::replacement();
        while (!stop_replacer.load(std::memory_order_acquire) &&
               !failed.load(std::memory_order_acquire)) {
            if (expired(deadline)) {
                note_failure();
                return;
            }
            // Replacing the newest entry is allowed to fail when the queue is
            // empty or a consumer already took it; the CAS race is under test.
            if (queue.try_replace_last(marker)) {
                replacements.fetch_add(1, std::memory_order_relaxed);
                replacement_seen.store(true, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    };

    auto consumer = [&](std::uint32_t id) {
        std::vector<observation<value_type>>& local = observations[id];
        local.reserve(per_producer * producer_count / consumer_count + 16);
        // Half the consumers use the predicate path, which must retry after a
        // concurrent replacement invalidates its CAS snapshot.  For the
        // checksummed payload this predicate also rejects a torn snapshot
        // instead of accepting it as a valid entry.
        const bool use_predicate = (id & 1U) == 0;
        auto accept = [](const value_type& candidate, Index) noexcept {
            return Payload::is_well_formed(candidate);
        };
        for (;;) {
            value_type value{};
            Index sequence{};
            const bool popped = use_predicate
                                    ? queue.try_pop_if(accept, value, sequence)
                                    : queue.try_pop(value, sequence);
            if (popped) {
                if (static_cast<std::uint64_t>(sequence) >= total) {
                    note_failure();
                    return;
                }
                if (!Payload::is_well_formed(value)) {
                    note_failure();
                    return;
                }
                const std::uint8_t previous =
                    seen[static_cast<std::size_t>(sequence)].fetch_add(
                        1, std::memory_order_acq_rel);
                if (previous != 0) {
                    note_failure();
                    return;
                }
                local.push_back(observation<value_type>{
                    static_cast<std::uint64_t>(sequence), value});
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
                    note_failure();
                    return;
                }
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(producer_count + consumer_count + 1);
    for (std::uint32_t i = 0; i < producer_count; ++i) {
        threads.emplace_back(producer, i);
    }
    std::thread replacer_thread(replacer);

    // With no consumers running yet, the newest publication cannot be taken,
    // so waiting for one successful replacement makes the replacement path a
    // deterministic part of the run instead of a scheduling accident.
    while (!replacement_seen.load(std::memory_order_acquire)) {
        if (failed.load(std::memory_order_acquire) || expired(deadline)) {
            note_failure();
            break;
        }
        std::this_thread::yield();
    }

    for (std::uint32_t i = 0; i < consumer_count; ++i) {
        threads.emplace_back(consumer, i);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    stop_replacer.store(true, std::memory_order_release);
    replacer_thread.join();

    const bool any_failure = failed.load(std::memory_order_acquire);
    CHECK(!any_failure);
    CHECK_EQ(consumed.load(std::memory_order_acquire), total);
    CHECK_EQ(produced.load(std::memory_order_acquire), total);
    CHECK(queue.empty());
    if (any_failure) {
        return; // keep the failure path bounded
    }

    std::vector<observation<value_type>> all;
    all.reserve(static_cast<std::size_t>(total));
    for (const auto& local : observations) {
        all.insert(all.end(), local.begin(), local.end());
    }
    std::sort(
        all.begin(), all.end(),
        [](const observation<value_type>& a, const observation<value_type>& b) {
            return a.sequence < b.sequence;
        });
    REQUIRE_EQ(all.size(), static_cast<std::size_t>(total));

    std::vector<bool> seen_sequence(static_cast<std::size_t>(total), false);
    std::vector<std::int64_t> last_item(producer_count, -1);
    for (std::size_t i = 0; i < all.size(); ++i) {
        CHECK_EQ(all[i].sequence, static_cast<std::uint64_t>(i));
        REQUIRE(all[i].sequence < total);
        CHECK(!seen_sequence[static_cast<std::size_t>(all[i].sequence)]);
        seen_sequence[static_cast<std::size_t>(all[i].sequence)] = true;

        const value_type& value = all[i].value;
        // Payload integrity: no torn cell snapshot may surface.  Every value
        // must be the exact original publication for this position or the
        // exact replacement marker.
        CHECK(Payload::is_well_formed(value));
        CHECK(Payload::is_intact(
            value, originals[static_cast<std::size_t>(all[i].sequence)]));
        if (Payload::is_replacement(value)) {
            continue;
        }

        std::uint32_t producer_id = 0;
        std::uint32_t item = 0;
        REQUIRE(Payload::decode(value, producer_id, item));
        REQUIRE(producer_id < producer_count);
        CHECK(item < per_producer);
        CHECK(static_cast<std::int64_t>(item) > last_item[producer_id]);
        last_item[producer_id] = static_cast<std::int64_t>(item);
    }
    for (bool flag : seen_sequence) {
        CHECK(flag);
    }
    for (const auto& count : seen) {
        CHECK_EQ(count.load(std::memory_order_acquire), std::uint8_t{1});
    }
    CHECK(replacements.load(std::memory_order_acquire) > 0);
}

template<class Payload, class Index, bool LazyPush, bool LazyPop>
void pop_if_rejects_rejected_replacement() {
    using value_type = typename Payload::value_type;
    using queue_type =
        mpmc::mpmc_queue<value_type, 1, Index, LazyPush, LazyPop>;

    queue_type queue;
    const auto deadline = clock_type::now() + std::chrono::seconds(10);

    const value_type accepted = Payload::make(0, 7);
    const value_type rejected = Payload::replacement();
    REQUIRE(Payload::predicate_accepts(accepted));
    REQUIRE(!Payload::predicate_accepts(rejected));
    REQUIRE(queue.try_push(accepted));

    // try_pop_if invokes the predicate after loading the cell and before its
    // compare-exchange.  Use that call point as the deterministic test hook:
    // signal the replacer, wait until the replacement is published, then let
    // try_pop_if proceed with its now-stale snapshot.  Stage 0 means wait,
    // 1 means replace, and 2 aborts without replacing.
    std::atomic<int> stage{0};
    std::atomic<bool> replacement_done{false};
    std::atomic<bool> replacement_applied{false};
    std::atomic<bool> timed_out{false};

    std::thread replacer([&] {
        for (;;) {
            const int current = stage.load(std::memory_order_acquire);
            if (current == 1) {
                replacement_applied.store(queue.try_replace(Index{0}, rejected),
                                          std::memory_order_release);
                replacement_done.store(true, std::memory_order_release);
                return;
            }
            if (current == 2) {
                return;
            }
            if (expired(deadline)) {
                timed_out.store(true, std::memory_order_release);
                return;
            }
            std::this_thread::yield();
        }
    });

    value_type popped = accepted;
    Index sequence{};
    int predicate_calls = 0;
    bool saw_rejected_candidate = false;
    auto accept = [&](const value_type& candidate, Index) noexcept {
        ++predicate_calls;
        if (!Payload::predicate_accepts(candidate)) {
            saw_rejected_candidate = true;
            if (stage.load(std::memory_order_acquire) == 0) {
                stage.store(2, std::memory_order_release);
            }
            return false;
        }
        if (stage.load(std::memory_order_acquire) == 0) {
            stage.store(1, std::memory_order_release);
        }
        while (!replacement_done.load(std::memory_order_acquire)) {
            if (expired(deadline)) {
                timed_out.store(true, std::memory_order_release);
                break;
            }
            std::this_thread::yield();
        }
        // The accepted candidate was replaced in the meantime, so the CAS in
        // try_pop_if must fail and the retry must observe the rejected value.
        return true;
    };

    const bool popped_ok = queue.try_pop_if(accept, popped, sequence);
    replacer.join();

    CHECK(!timed_out.load(std::memory_order_acquire));
    CHECK(replacement_applied.load(std::memory_order_acquire));
    CHECK(saw_rejected_candidate);
    CHECK(predicate_calls >= 2);
    // The rejected replacement must never be reported as an accepted pop.
    CHECK(!popped_ok);
    CHECK(!Payload::is_replacement(popped));
    CHECK(popped == accepted);

    // The rejected replacement is still queued and is returned by a plain pop
    // with its payload intact.
    value_type remaining = accepted;
    Index remaining_sequence{};
    REQUIRE(queue.try_pop(remaining, remaining_sequence));
    CHECK(remaining == rejected);
    CHECK(Payload::is_well_formed(remaining));
    CHECK_EQ(remaining_sequence, Index{0});
    CHECK(queue.empty());
}

template<bool Has16> void run_lossless_matrix() {
    // Capacity 1 is the tightest wraparound case.
    lossless_matrix<std::uint32_t, std::uint32_t, 1, false, false>(3, 3, 1000);
    lossless_matrix<std::uint32_t, std::uint32_t, 1, true, true>(3, 3, 1000);

    lossless_matrix<std::uint32_t, std::uint32_t, 8, false, false>(2, 2, 2000);
    lossless_matrix<std::uint32_t, std::uint32_t, 8, true, false>(2, 2, 2000);
    lossless_matrix<std::uint32_t, std::uint32_t, 8, false, true>(2, 2, 2000);
    lossless_matrix<std::uint32_t, std::uint32_t, 8, true, true>(2, 2, 2000);

    lossless_matrix<std::uint32_t, std::uint32_t, 64, false, false>(4, 4, 2000);
    lossless_matrix<std::uint32_t, std::uint32_t, 64, true, true>(4, 4, 2000);

    if constexpr (Has16) {
        lossless_matrix<std::uint64_t, std::uint64_t, 1, false, false>(3, 3,
                                                                       1000);
        lossless_matrix<std::uint64_t, std::uint64_t, 1, true, true>(3, 3,
                                                                     1000);

        lossless_matrix<std::uint64_t, std::uint64_t, 8, false, false>(2, 2,
                                                                       2000);
        lossless_matrix<std::uint64_t, std::uint64_t, 8, true, false>(2, 2,
                                                                      2000);
        lossless_matrix<std::uint64_t, std::uint64_t, 8, false, true>(2, 2,
                                                                      2000);
        lossless_matrix<std::uint64_t, std::uint64_t, 8, true, true>(2, 2,
                                                                     2000);

        lossless_matrix<std::uint64_t, std::uint64_t, 64, false, false>(4, 4,
                                                                        2000);
        lossless_matrix<std::uint64_t, std::uint64_t, 64, true, true>(4, 4,
                                                                      2000);
    }
}

template<bool Has16> void run_replace_matrix() {
    replace_pop_if_race<encoded_word_payload, std::uint32_t, 16, false, false>(
        2, 2, 5000);
    replace_pop_if_race<encoded_word_payload, std::uint32_t, 16, true, true>(
        2, 2, 5000);

    pop_if_rejects_rejected_replacement<encoded_word_payload, std::uint32_t,
                                        false, false>();
    pop_if_rejects_rejected_replacement<encoded_word_payload, std::uint32_t,
                                        true, true>();

    if constexpr (Has16) {
        // 12-byte checksummed payload + 32-bit index = one 16-byte cell, so a
        // torn snapshot surfaces as a checksum/integrity failure.
        replace_pop_if_race<checksummed_cell_payload, std::uint32_t, 16, false,
                            false>(2, 2, 5000);
        replace_pop_if_race<checksummed_cell_payload, std::uint32_t, 16, true,
                            true>(2, 2, 5000);

        pop_if_rejects_rejected_replacement<checksummed_cell_payload,
                                            std::uint32_t, false, false>();
        pop_if_rejects_rejected_replacement<checksummed_cell_payload,
                                            std::uint32_t, true, true>();
    }
}

} // namespace

MPMC_TEST(concurrency_lossless_cell_matrix) {
    run_lossless_matrix<mpmc_test::has_lock_free_16_byte_cell>();
}

MPMC_TEST(concurrency_replace_pop_if_matrix) {
    run_replace_matrix<mpmc_test::has_lock_free_16_byte_cell>();
}

MPMC_TEST(narrow_index_concurrent_exhaustion) {
    using queue_type = mpmc::mpmc_queue<std::uint8_t, 2, std::uint8_t>;
    queue_type queue;

    constexpr std::uint32_t producer_count = 3;
    constexpr std::uint32_t consumer_count = 3;
    constexpr std::uint32_t per_producer = 40;
    constexpr std::uint32_t total = producer_count * per_producer;
    static_assert(total < 126);

    std::vector<std::atomic<std::uint8_t>> seen(126);
    for (auto& count : seen) {
        count.store(0, std::memory_order_relaxed);
    }
    std::atomic<std::uint32_t> produced{0};
    std::atomic<std::uint32_t> consumed{0};
    std::atomic<bool> failed{false};
    const auto deadline = clock_type::now() + std::chrono::seconds(30);

    std::vector<std::thread> threads;
    threads.reserve(producer_count + consumer_count);
    for (std::uint32_t p = 0; p < producer_count; ++p) {
        threads.emplace_back([&, p] {
            for (std::uint32_t i = 0; i < per_producer; ++i) {
                const std::uint8_t value =
                    static_cast<std::uint8_t>(p * per_producer + i);
                while (!queue.try_push(value)) {
                    if (failed.load(std::memory_order_acquire) ||
                        expired(deadline)) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    std::this_thread::yield();
                }
                produced.fetch_add(1, std::memory_order_release);
            }
        });
    }
    for (std::uint32_t c = 0; c < consumer_count; ++c) {
        threads.emplace_back([&] {
            for (;;) {
                std::uint8_t value = 0;
                std::uint8_t sequence = 0;
                if (queue.try_pop(value, sequence)) {
                    if (sequence >= 126) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    if (seen[sequence].fetch_add(
                            1, std::memory_order_acq_rel) != 0) {
                        failed.store(true, std::memory_order_release);
                        return;
                    }
                    const std::uint32_t count =
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
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(!failed.load(std::memory_order_acquire));
    CHECK_EQ(consumed.load(std::memory_order_acquire), total);
    CHECK_EQ(produced.load(std::memory_order_acquire), total);
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(total); ++i) {
        CHECK_EQ(seen[i].load(std::memory_order_acquire), std::uint8_t{1});
    }

    // Drain the remaining publication positions sequentially, then verify the
    // fail-stop horizon is reached exactly at publication_limit().
    for (std::uint8_t value = static_cast<std::uint8_t>(total); value < 126;
         ++value) {
        REQUIRE(queue.try_push(value));
        std::uint8_t popped = 0;
        REQUIRE(queue.try_pop(popped));
        CHECK_EQ(popped, value);
    }
    CHECK_EQ(queue.publication_limit(), std::uint8_t{126});
    CHECK(queue.sequence_exhausted());
    CHECK(!queue.try_push(std::uint8_t{0}));
    CHECK(queue.empty());
}
