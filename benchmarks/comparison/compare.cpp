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
// Cross-library bounded MPMC queue comparison benchmark.
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <pthread.h>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sched.h>
#endif

#include <boost/lockfree/queue.hpp>
#include <boost/version.hpp>
#include <mpmc/mpmc_queue.hpp>
#include <mpmc/version.hpp>
#include <rigtorp/MPMCQueue.h>

namespace {

using value_type = std::uint64_t;
using clock_type = std::chrono::steady_clock;

constexpr std::size_t kThroughputCapacity = 4096;
constexpr std::size_t kLatencyCapacity = 64;
constexpr std::uint64_t kMaxTotalItems = 134'217'728;
constexpr std::uint64_t kCalibrationPilotItems = 8'388'608;
constexpr value_type kBenchmarkWarmupBit = value_type{1} << 63U;
constexpr int kPrimaryRounds = 18;
constexpr int kStressRounds = 6;
constexpr std::size_t kDefaultLatencySamples = 1'000'000;
constexpr std::size_t kLatencyWarmup = 100'000;
constexpr std::uint64_t kSentinelIterations = 50'000'000;
constexpr std::uint64_t kRngSeed = 0x4d504d43ULL;

std::atomic<int> placement_failures{0};

// macOS exposes no usable hard thread-affinity policy on Apple silicon.  The
// user-interactive QoS class is a scheduler hint, not CPU pinning.  Linux pins
// each worker to a distinct allowed CPU and verifies the resulting mask.
bool apply_thread_placement(unsigned slot) {
#if defined(__APPLE__)
    (void)slot;
    return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#elif defined(__linux__)
    cpu_set_t allowed;
    if (pthread_getaffinity_np(pthread_self(), sizeof(allowed), &allowed) !=
        0) {
        return false;
    }
    std::array<int, CPU_SETSIZE> cpus{};
    std::size_t count = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
        if (CPU_ISSET(i, &allowed)) {
            cpus[count++] = i;
        }
    }
    if (count == 0) {
        return false;
    }
    const int selected_cpu = cpus[slot % count];
    cpu_set_t selected;
    CPU_ZERO(&selected);
    CPU_SET(selected_cpu, &selected);
    if (pthread_setaffinity_np(pthread_self(), sizeof(selected), &selected) !=
        0) {
        return false;
    }
    cpu_set_t verified;
    CPU_ZERO(&verified);
    if (pthread_getaffinity_np(pthread_self(), sizeof(verified), &verified) !=
        0) {
        return false;
    }
    return CPU_COUNT(&verified) == 1 && CPU_ISSET(selected_cpu, &verified);
#else
    (void)slot;
    return true;
#endif
}

void report_placement_failure() {
    placement_failures.fetch_add(1, std::memory_order_relaxed);
}

double load_average_1m() {
    double value = 0.0;
    return getloadavg(&value, 1) == 1 ? value : -1.0;
}

double g_peak_primary_load = 0.0;
double g_peak_stress_load = 0.0;

void observe_load(double& peak) {
    const double load = load_average_1m();
    if (load > peak) {
        peak = load;
    }
}

const char* placement_description() {
#if defined(__APPLE__)
    // Configuration is read before worker threads start.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (std::getenv("MPMC_TASKPOLICY") != nullptr) {
        return "macOS user-interactive QoS with taskpolicy latency tier; "
               "scheduler-managed, no hard CPU affinity";
    }
    return "macOS user-interactive QoS; scheduler-managed, no hard CPU "
           "affinity";
#elif defined(__linux__)
    return "Linux per-thread CPU affinity, verified";
#else
    return "no thread placement";
#endif
}

// A failed try operation retries with a non-blocking architecture hint.  No
// syscall or sleep is placed in the measured retry loop.
inline void spin_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    // NOLINTNEXTLINE(portability-no-assembler)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

template<std::size_t Capacity>
using ours_queue = mpmc::mpmc_queue<value_type, Capacity, std::uint32_t>;
template<std::size_t Capacity> using rig_queue = rigtorp::MPMCQueue<value_type>;
template<std::size_t Capacity>
using boost_queue =
    boost::lockfree::queue<value_type, boost::lockfree::capacity<Capacity>>;

template<class Q> bool queue_push(Q& q, value_type v) {
    if constexpr (requires { q.try_push(v); }) {
        return q.try_push(v);
    } else {
        return q.push(v);
    }
}

template<class Q> bool queue_pop(Q& q, value_type& v) {
    if constexpr (requires { q.try_pop(v); }) {
        return q.try_pop(v);
    } else {
        return q.pop(v);
    }
}

template<class T> struct is_boost_capacity : std::false_type {};
template<std::size_t Capacity>
struct is_boost_capacity<boost::lockfree::capacity<Capacity>> : std::true_type {
};

template<class... Options>
inline constexpr bool has_boost_capacity_v =
    (is_boost_capacity<Options>::value || ...);

template<class Q> struct queue_factory {
    static std::unique_ptr<Q> make(std::size_t capacity) {
        if constexpr (std::is_constructible_v<Q, std::size_t>) {
            return std::make_unique<Q>(capacity);
        } else {
            (void)capacity;
            return std::make_unique<Q>();
        }
    }
};

// Boost's fixed-capacity constructor has no size parameter, while its
// variable-capacity constructor does.  Do not let is_constructible choose the
// wrong overload on older Boost releases, where the unused constructor is not
// removed by a C++20 constraint: an assertion-enabled build would abort.
template<class T, class... Options>
struct queue_factory<boost::lockfree::queue<T, Options...>> {
    static std::unique_ptr<boost::lockfree::queue<T, Options...>>
    make(std::size_t capacity) {
        if constexpr (has_boost_capacity_v<Options...>) {
            (void)capacity;
            return std::make_unique<boost::lockfree::queue<T, Options...>>();
        } else {
            return std::make_unique<boost::lockfree::queue<T, Options...>>(
                capacity);
        }
    }
};

template<class Q> std::unique_ptr<Q> make_queue(std::size_t capacity) {
    return queue_factory<Q>::make(capacity);
}

struct throughput_result {
    double rate_mops = 0.0;
    double seconds = 0.0;
    double start_skew_seconds = 0.0;
    std::uint64_t push_failures = 0;
    std::uint64_t pop_failures = 0;
    bool values_ok = false;
    bool ok = false;
};

struct latency_result {
    double mean_ns = 0.0;
    double p50_ns = 0.0;
    double p90_ns = 0.0;
    double p99_ns = 0.0;
    double p999_ns = 0.0;
    double max_ns = 0.0;
    bool ok = false;
};

// One statistics block per consumer, padded to the largest cache line in the
// supported benchmark targets (Apple silicon uses 128-byte lines).  The local
// bitmaps are allocated separately; hot-path updates never touch a shared
// atomic.
struct alignas(128) consumer_stats {
    std::uint64_t count = 0;
    std::uint64_t checksum = 0;
    std::uint64_t checksum_squares = 0;
    std::vector<std::uint64_t> seen;
    bool duplicate = false;
    bool invalid = false;
};
static_assert(alignof(consumer_stats) == 128);

struct alignas(128) attempt_stats {
    std::uint64_t value = 0;
};

struct alignas(128) time_point_slot {
    clock_type::time_point value{};
};

struct topology {
    int producers;
    int consumers;
};

constexpr std::array<topology, 5> kPrimaryTopologies{
    {{1, 1}, {2, 2}, {4, 4}, {4, 1}, {1, 4}}};
constexpr topology kStressTopology{8, 8};
constexpr std::array<topology, 6> kAllTopologies{
    {{1, 1}, {2, 2}, {4, 4}, {4, 1}, {1, 4}, {8, 8}}};
using workload_table = std::array<std::array<std::uint64_t, 6>, 3>;

std::uint64_t expected_checksum(int producers, std::uint64_t items) {
    const std::uint64_t triangular = items * (items - 1) / 2;
    std::uint64_t total = static_cast<std::uint64_t>(producers) * triangular;
    for (int p = 0; p < producers; ++p) {
        total += items * (static_cast<std::uint64_t>(p) << 32U);
    }
    return total;
}

std::uint64_t expected_sum_of_squares(int producers, std::uint64_t items) {
    using wide = unsigned __int128;
    const wide triangular = static_cast<wide>(items) * (items - 1) / 2;
    const wide square_sum = static_cast<wide>(items) * (items - 1) *
                            (2 * static_cast<wide>(items) - 1) / 6;
    wide base_sum = 0;
    wide base_square_sum = 0;
    for (int p = 0; p < producers; ++p) {
        const wide base = static_cast<wide>(p) << 32U;
        base_sum += base;
        base_square_sum += base * base;
    }
    const wide total = static_cast<wide>(producers) * square_sum +
                       2 * base_sum * triangular +
                       static_cast<wide>(items) * base_square_sum;
    return static_cast<std::uint64_t>(total);
}

template<class Q, bool Exact>
throughput_result run_throughput_once(Q& q, int producers, int consumers,
                                      std::uint64_t total_items,
                                      std::uint64_t warm_items_total) {
    if (total_items == 0 || total_items % 64 != 0 ||
        total_items % static_cast<std::uint64_t>(producers) != 0 ||
        warm_items_total % static_cast<std::uint64_t>(producers) != 0) {
        throw std::runtime_error("invalid throughput workload size");
    }
    const auto target = total_items;
    const auto items_per_producer =
        target / static_cast<std::uint64_t>(producers);
    const auto warm_per_producer =
        warm_items_total / static_cast<std::uint64_t>(producers);
    const std::size_t words =
        Exact ? static_cast<std::size_t>(target / 64) : std::size_t{0};
    const auto run_deadline = clock_type::now() + std::chrono::seconds(300);
    std::atomic<bool> aborted{false};
    auto abort_now = [&aborted, run_deadline] {
        // Failed queue operations are the hot retry path.  Reading the clock
        // there would add a hidden, library-dependent backoff, so check the
        // deadline once every 64 failures on each worker.
        thread_local std::uint32_t checks = 0;
        if (aborted.load(std::memory_order_relaxed)) {
            return true;
        }
        if ((++checks & 63U) != 0U) {
            return false;
        }
        if (clock_type::now() > run_deadline) {
            aborted.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    };
    std::atomic<int> ready{0};
    std::atomic<bool> begin_warmup{false};
    std::atomic<int> warm_producers_done{0};
    std::atomic<int> warm_consumers_done{0};
    std::atomic<bool> timed_go{false};
    std::atomic<int> timed_producers_done{0};
    std::atomic<bool> warmup_invalid{false};
    std::vector<consumer_stats> stats(static_cast<std::size_t>(consumers));
    if constexpr (Exact) {
        for (auto& s : stats) {
            s.seen.assign(words, 0);
        }
    }
    std::vector<time_point_slot> starts(
        static_cast<std::size_t>(producers + consumers));
    std::vector<time_point_slot> finishes(
        static_cast<std::size_t>(producers + consumers));
    std::vector<attempt_stats> push_failures(
        static_cast<std::size_t>(producers));
    std::vector<attempt_stats> pop_failures(
        static_cast<std::size_t>(consumers));

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers) +
                    static_cast<std::size_t>(consumers));

    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            if (!apply_thread_placement(static_cast<unsigned>(p))) {
                report_placement_failure();
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!begin_warmup.load(std::memory_order_acquire)) {
                spin_relax();
            }
            const std::uint64_t base = static_cast<std::uint64_t>(p) << 32U;
            for (std::uint64_t i = 0; i < warm_per_producer; ++i) {
                if (aborted.load(std::memory_order_relaxed)) {
                    return;
                }
                const value_type v = kBenchmarkWarmupBit | base | i;
                while (!queue_push(q, v)) {
                    if (abort_now()) {
                        return;
                    }
                    spin_relax();
                }
            }
            warm_producers_done.fetch_add(1, std::memory_order_release);
            while (warm_consumers_done.load(std::memory_order_acquire) !=
                   consumers) {
                spin_relax();
            }
            while (!timed_go.load(std::memory_order_acquire)) {
                spin_relax();
            }
            starts[static_cast<std::size_t>(p)].value = clock_type::now();
            auto& failed = push_failures[static_cast<std::size_t>(p)];
            for (std::uint64_t i = 0; i < items_per_producer; ++i) {
                if (aborted.load(std::memory_order_relaxed)) {
                    return;
                }
                const value_type v = base | i;
                while (!queue_push(q, v)) {
                    ++failed.value;
                    if (abort_now()) {
                        return;
                    }
                    spin_relax();
                }
            }
            finishes[static_cast<std::size_t>(p)].value = clock_type::now();
            timed_producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    for (int c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            const auto slot = static_cast<std::size_t>(producers) +
                              static_cast<std::size_t>(c);
            if (!apply_thread_placement(static_cast<unsigned>(slot))) {
                report_placement_failure();
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!begin_warmup.load(std::memory_order_acquire)) {
                spin_relax();
            }
            value_type warm_value = 0;
            auto check_warmup_value = [&warmup_invalid, producers,
                                       warm_per_producer](value_type v) {
                const auto producer =
                    static_cast<std::uint64_t>(v >> 32U) & 0x7fffffffULL;
                const auto item = static_cast<std::uint64_t>(v & 0xffffffffULL);
                if ((v & kBenchmarkWarmupBit) == 0 ||
                    producer >= static_cast<std::uint64_t>(producers) ||
                    item >= warm_per_producer) {
                    warmup_invalid.store(true, std::memory_order_relaxed);
                }
            };
            while (warm_producers_done.load(std::memory_order_acquire) !=
                   producers) {
                if (queue_pop(q, warm_value)) {
                    check_warmup_value(warm_value);
                    continue;
                }
                if (abort_now()) {
                    return;
                }
                spin_relax();
            }
            while (queue_pop(q, warm_value)) {
                check_warmup_value(warm_value);
            }
            if (aborted.load(std::memory_order_relaxed)) {
                return;
            }
            warm_consumers_done.fetch_add(1, std::memory_order_release);
            while (!timed_go.load(std::memory_order_acquire)) {
                spin_relax();
            }
            starts[slot].value = clock_type::now();
            auto& s = stats[static_cast<std::size_t>(c)];
            auto& failed = pop_failures[static_cast<std::size_t>(c)];
            auto record = [&s, producers, items_per_producer](value_type v) {
                ++s.count;
                s.checksum += v;
                s.checksum_squares += v * v;
                const auto producer = static_cast<std::uint64_t>(v >> 32U);
                const auto item = static_cast<std::uint64_t>(v & 0xffffffffULL);
                if (producer >= static_cast<std::uint64_t>(producers) ||
                    item >= items_per_producer) {
                    s.invalid = true;
                    return;
                }
                if constexpr (Exact) {
                    const auto bit = producer * items_per_producer + item;
                    auto& word = s.seen[static_cast<std::size_t>(bit >> 6U)];
                    const auto mask = std::uint64_t{1} << (bit & 63U);
                    if ((word & mask) != 0) {
                        s.duplicate = true;
                    }
                    word |= mask;
                }
            };
            for (;;) {
                value_type v = 0;
                if (queue_pop(q, v)) {
                    record(v);
                    continue;
                }
                ++failed.value;
                if (abort_now()) {
                    return;
                }
                if (timed_producers_done.load(std::memory_order_acquire) ==
                    producers) {
                    while (queue_pop(q, v)) {
                        record(v);
                    }
                    break;
                }
                spin_relax();
            }
            finishes[slot].value = clock_type::now();
        });
    }

    while (ready.load(std::memory_order_acquire) < producers + consumers) {
        if (abort_now()) {
            break;
        }
        spin_relax();
    }
    begin_warmup.store(true, std::memory_order_release);
    while (warm_consumers_done.load(std::memory_order_acquire) != consumers) {
        if (abort_now()) {
            break;
        }
        spin_relax();
    }
    const auto t0 = clock_type::now();
    timed_go.store(true, std::memory_order_release);
    for (auto& t : threads) {
        t.join();
    }
    if (aborted.load(std::memory_order_relaxed)) {
        throw std::runtime_error(
            "throughput trial exceeded the 300 s deadline");
    }
    if (warmup_invalid.load(std::memory_order_relaxed)) {
        throw std::runtime_error("throughput warm-up validation failed");
    }

    auto earliest_start = starts.front().value;
    auto latest_start = starts.front().value;
    auto latest_finish = finishes.front().value;
    for (const auto& start : starts) {
        earliest_start = std::min(earliest_start, start.value);
        latest_start = std::max(latest_start, start.value);
    }
    for (const auto& finish : finishes) {
        latest_finish = std::max(latest_finish, finish.value);
    }

    std::uint64_t count = 0;
    std::uint64_t checksum = 0;
    std::uint64_t checksum_squares = 0;
    bool duplicate = false;
    bool invalid = false;
    std::vector<std::uint64_t> union_seen;
    if constexpr (Exact) {
        union_seen.assign(words, 0);
    }
    for (const auto& s : stats) {
        count += s.count;
        checksum += s.checksum;
        checksum_squares += s.checksum_squares;
        duplicate = duplicate || s.duplicate;
        invalid = invalid || s.invalid;
        if constexpr (Exact) {
            for (std::size_t w = 0; w < words; ++w) {
                if ((union_seen[w] & s.seen[w]) != 0) {
                    duplicate = true;
                }
                union_seen[w] |= s.seen[w];
            }
        }
    }

    bool missing = false;
    if constexpr (Exact) {
        for (std::size_t w = 0; w < words; ++w) {
            const auto remaining = target - static_cast<std::uint64_t>(w) * 64;
            const auto mask = remaining >= 64
                                  ? ~std::uint64_t{0}
                                  : ((std::uint64_t{1} << remaining) - 1);
            if ((union_seen[w] & mask) != mask) {
                missing = true;
            }
        }
    }

    bool residual = false;
    value_type leftover = 0;
    while (queue_pop(q, leftover)) {
        residual = true;
    }

    throughput_result r;
    r.seconds = std::chrono::duration<double>(latest_finish - t0).count();
    r.start_skew_seconds =
        std::chrono::duration<double>(latest_start - earliest_start).count();
    r.rate_mops =
        r.seconds > 0.0 ? static_cast<double>(count) / r.seconds / 1.0e6 : 0.0;
    for (const auto& failed : push_failures) {
        r.push_failures += failed.value;
    }
    for (const auto& failed : pop_failures) {
        r.pop_failures += failed.value;
    }
    const bool count_and_sums_ok =
        (count == target) &&
        (checksum == expected_checksum(producers, items_per_producer)) &&
        (checksum_squares ==
         expected_sum_of_squares(producers, items_per_producer));
    if constexpr (Exact) {
        r.values_ok = count_and_sums_ok && !duplicate && !missing && !invalid &&
                      !residual;
    } else {
        r.values_ok = count_and_sums_ok && !invalid && !residual;
    }
    r.ok = r.values_ok;
    return r;
}

template<class Q>
latency_result run_latency_once(std::size_t samples, std::size_t warmup) {
    constexpr auto kLatencyDeadline = std::chrono::seconds(300);
    const auto deadline = clock_type::now() + kLatencyDeadline;
    auto q1 = make_queue<Q>(kLatencyCapacity);
    auto q2 = make_queue<Q>(kLatencyCapacity);
    std::atomic<bool> ready{false};
    std::atomic<bool> go{false};
    std::atomic<bool> aborted{false};

    auto check_deadline = [&](std::uint32_t& spin) {
        if ((++spin & 63U) == 0U && clock_type::now() > deadline) {
            aborted.store(true, std::memory_order_relaxed);
            return false;
        }
        return true;
    };

    std::thread responder([&] {
        if (!apply_thread_placement(1)) {
            report_placement_failure();
        }
        ready.store(true, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::uint32_t spin = 0;
        for (;;) {
            value_type v = 0;
            if (!queue_pop(*q1, v)) {
                if (!check_deadline(spin)) {
                    return;
                }
                spin_relax();
                continue;
            }
            if (v == std::numeric_limits<value_type>::max()) {
                break;
            }
            while (!queue_push(*q2, v)) {
                if (!check_deadline(spin)) {
                    return;
                }
                spin_relax();
            }
        }
    });
    struct joiner {
        std::thread& thread;
        ~joiner() {
            if (thread.joinable()) {
                thread.join();
            }
        }
    } join_guard{responder};

    while (!ready.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);

    std::uint32_t spin = 0;
    for (std::size_t i = 0; i < warmup; ++i) {
        while (!queue_push(*q1, static_cast<value_type>(i))) {
            if (!check_deadline(spin)) {
                throw std::runtime_error("latency trial exceeded 300 s");
            }
            spin_relax();
        }
        value_type v = 0;
        while (!queue_pop(*q2, v)) {
            if (!check_deadline(spin)) {
                throw std::runtime_error("latency trial exceeded 300 s");
            }
            spin_relax();
        }
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(samples);
    bool valid = true;
    for (std::size_t i = 0; i < samples; ++i) {
        const auto t0 = clock_type::now();
        while (!queue_push(*q1, static_cast<value_type>(i))) {
            if (!check_deadline(spin)) {
                throw std::runtime_error("latency trial exceeded 300 s");
            }
            spin_relax();
        }
        value_type v = 0;
        while (!queue_pop(*q2, v)) {
            if (!check_deadline(spin)) {
                throw std::runtime_error("latency trial exceeded 300 s");
            }
            spin_relax();
        }
        const auto t1 = clock_type::now();
        valid = valid && (v == static_cast<value_type>(i));
        const auto ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count());
        latencies.push_back(ns);
    }
    while (!queue_push(*q1, std::numeric_limits<value_type>::max())) {
        if (!check_deadline(spin)) {
            throw std::runtime_error("latency trial exceeded 300 s");
        }
        spin_relax();
    }
    responder.join();
    if (aborted.load(std::memory_order_relaxed)) {
        throw std::runtime_error("latency trial exceeded 300 s");
    }

    std::sort(latencies.begin(), latencies.end());
    latency_result r;
    std::uint64_t sum = 0;
    for (auto v : latencies) {
        sum += v;
    }
    auto percentile = [&](double p) {
        if (latencies.empty()) {
            return 0.0;
        }
        const auto rank = static_cast<std::size_t>(
            std::ceil(p * static_cast<double>(latencies.size())));
        const auto idx = std::min(latencies.size() - 1,
                                  rank == 0 ? std::size_t{0} : rank - 1);
        return static_cast<double>(latencies[idx]);
    };
    r.mean_ns =
        static_cast<double>(sum) / static_cast<double>(latencies.size());
    r.p50_ns = percentile(0.50);
    r.p90_ns = percentile(0.90);
    r.p99_ns = percentile(0.99);
    r.p999_ns = percentile(0.999);
    r.max_ns = static_cast<double>(latencies.back());
    r.ok = valid;
    return r;
}

template<class Q>
latency_result run_latency_sample(std::size_t samples, std::size_t warmup) {
    return run_latency_once<Q>(samples, warmup);
}

struct latency_queue_spec {
    const char* name;
    latency_result (*run)(std::size_t, std::size_t);
};

const std::array<latency_queue_spec, 3> kLatencyQueues{{
    {"mpmc-16B", &run_latency_sample<ours_queue<kLatencyCapacity>>},
    {"rigtorp", &run_latency_sample<rig_queue<kLatencyCapacity>>},
    {"boost", &run_latency_sample<boost_queue<kLatencyCapacity>>},
}};

struct throughput_queue_spec {
    const char* name;
    throughput_result (*run)(int, int, std::uint64_t, std::uint64_t);
    throughput_result (*run_exact)(int, int, std::uint64_t, std::uint64_t);
};

template<class Q, bool Exact>
throughput_result run_throughput_sample(int producers, int consumers,
                                        std::uint64_t total_items,
                                        std::uint64_t warm_items_total) {
    auto q = make_queue<Q>(kThroughputCapacity);
    return run_throughput_once<Q, Exact>(*q, producers, consumers, total_items,
                                         warm_items_total);
}

const std::array<throughput_queue_spec, 3> kThroughputQueues{{
    {"mpmc-16B", &run_throughput_sample<ours_queue<kThroughputCapacity>, false>,
     &run_throughput_sample<ours_queue<kThroughputCapacity>, true>},
    {"rigtorp", &run_throughput_sample<rig_queue<kThroughputCapacity>, false>,
     &run_throughput_sample<rig_queue<kThroughputCapacity>, true>},
    {"boost", &run_throughput_sample<boost_queue<kThroughputCapacity>, false>,
     &run_throughput_sample<boost_queue<kThroughputCapacity>, true>},
}};

// Six permutations of three eligible libraries, repeated three times for 18
// balanced primary rounds.  Each library appears six times in each position.
constexpr std::array<std::array<int, 3>, 6> kOrders{{
    {{0, 1, 2}},
    {{0, 2, 1}},
    {{1, 0, 2}},
    {{1, 2, 0}},
    {{2, 0, 1}},
    {{2, 1, 0}},
}};

double median_of(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0
                                  : values[middle];
}

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

std::uint64_t warmup_items(std::uint64_t total_items, int producers) {
    const auto unit = static_cast<std::uint64_t>(producers) * 64ULL;
    auto warm = std::min<std::uint64_t>(total_items, 65536ULL);
    warm = (warm / unit) * unit;
    if (warm == 0) {
        warm = unit;
    }
    return warm;
}

void print_throughput_trial(const char* classification,
                            const throughput_queue_spec& spec,
                            const topology& topo, std::uint64_t total_items,
                            int round, const throughput_result& result) {
    std::printf("{\"kind\":\"throughput\",\"classification\":\"%s\","
                "\"library\":\"%s\",\"producers\":%d,\"consumers\":%d,"
                "\"capacity\":%zu,\"round\":%d,\"total_items\":%llu,"
                "\"elapsed_seconds\":%.9f,\"rate_Mitems_s\":%.6f,"
                "\"start_skew_seconds\":%.9f,\"push_failures\":%llu,"
                "\"pop_failures\":%llu,\"validation\":\"%s\"}\n",
                classification, spec.name, topo.producers, topo.consumers,
                kThroughputCapacity, round,
                static_cast<unsigned long long>(total_items), result.seconds,
                result.rate_mops, result.start_skew_seconds,
                static_cast<unsigned long long>(result.push_failures),
                static_cast<unsigned long long>(result.pop_failures),
                result.values_ok ? "ok" : "mismatch");
}

void run_exact_validation() {
    constexpr std::uint64_t kExactItems = 1'048'576;
    for (const auto& spec : kThroughputQueues) {
        for (const auto& topo : kAllTopologies) {
            const auto result =
                spec.run_exact(topo.producers, topo.consumers, kExactItems, 0);
            std::printf("{\"kind\":\"exact\",\"library\":\"%s\","
                        "\"producers\":%d,\"consumers\":%d,"
                        "\"total_items\":%llu,\"validation\":\"%s\"}\n",
                        spec.name, topo.producers, topo.consumers,
                        static_cast<unsigned long long>(kExactItems),
                        result.values_ok ? "ok" : "mismatch");
            if (!result.values_ok) {
                throw std::runtime_error(
                    std::string("exact validation failed for ") + spec.name);
            }
        }
    }
}

void print_latency_trial(const latency_queue_spec& spec, int round,
                         std::size_t samples, const latency_result& result) {
    std::printf("{\"kind\":\"latency\",\"library\":\"%s\",\"round\":%d,"
                "\"samples\":%zu,\"mean_ns\":%.3f,\"p50_ns\":%.3f,"
                "\"p90_ns\":%.3f,\"p99_ns\":%.3f,\"p999_ns\":%.3f,"
                "\"max_ns\":%.3f,\"validation\":\"%s\"}\n",
                spec.name, round, samples, result.mean_ns, result.p50_ns,
                result.p90_ns, result.p99_ns, result.p999_ns, result.max_ns,
                result.ok ? "ok" : "mismatch");
}

double sentinel_sample();

void run_throughput_rounds(const workload_table& workloads, int rounds,
                           bool quick, double sentinel_baseline) {
    for (int round = 0; round < rounds; ++round) {
        const auto& order = kOrders[static_cast<std::size_t>(round) % 6];
        std::array<int, 5> topology_order{{0, 1, 2, 3, 4}};
        std::mt19937 rng(static_cast<std::uint32_t>(kRngSeed) ^
                         static_cast<std::uint32_t>(round));
        std::shuffle(topology_order.begin(), topology_order.end(), rng);
        for (int topology_index : topology_order) {
            const auto& topo =
                kPrimaryTopologies[static_cast<std::size_t>(topology_index)];
            for (int position = 0; position < 3; ++position) {
                const auto index = static_cast<std::size_t>(order[position]);
                const auto& spec = kThroughputQueues[index];
                const auto total_items =
                    workloads[index][static_cast<std::size_t>(topology_index)];
                const auto warm =
                    quick ? 0 : warmup_items(total_items, topo.producers);
                const auto result =
                    spec.run(topo.producers, topo.consumers, total_items, warm);
                observe_load(g_peak_primary_load);
                print_throughput_trial("primary", spec, topo, total_items,
                                       round, result);
                if (!result.values_ok) {
                    throw std::runtime_error(
                        std::string("throughput validation failed for ") +
                        spec.name);
                }
                if (!quick && result.seconds < 0.250) {
                    throw std::runtime_error(
                        std::string("timed trial shorter than 250 ms for ") +
                        spec.name);
                }
                if (!quick && result.seconds > 0.0 &&
                    result.start_skew_seconds > 0.01 * result.seconds) {
                    throw std::runtime_error(
                        std::string("start skew above 1% for ") + spec.name);
                }
                if (!quick) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        }
        if (!quick) {
            const auto observed = sentinel_sample();
            std::printf("{\"kind\":\"sentinel\",\"round\":%d,"
                        "\"seconds\":%.9f}\n",
                        round, observed);
            if (sentinel_baseline <= 0.0 ||
                std::abs(observed - sentinel_baseline) / sentinel_baseline >
                    0.10) {
                throw std::runtime_error(
                    "host timing drift exceeded 10% in round " +
                    std::to_string(round));
            }
        }
    }

    if (!quick) {
        for (int round = 0; round < kStressRounds; ++round) {
            const auto& order = kOrders[static_cast<std::size_t>(round) % 6];
            for (int position = 0; position < 3; ++position) {
                const auto index = static_cast<std::size_t>(order[position]);
                const auto& spec = kThroughputQueues[index];
                const auto total_items = workloads[index][5];
                const auto warm =
                    warmup_items(total_items, kStressTopology.producers);
                const auto result =
                    spec.run(kStressTopology.producers,
                             kStressTopology.consumers, total_items, warm);
                observe_load(g_peak_stress_load);
                print_throughput_trial("stress", spec, kStressTopology,
                                       total_items, round, result);
                if (!result.values_ok) {
                    throw std::runtime_error(
                        std::string("throughput validation failed for ") +
                        spec.name);
                }
                if (result.seconds < 0.250) {
                    throw std::runtime_error(
                        std::string("timed trial shorter than 250 ms for ") +
                        spec.name);
                }
                // The 8P/8C case intentionally oversubscribes this host and
                // is a scheduler-stress measurement.  A late worker start is
                // part of that stress, so the primary 1% skew gate does not
                // apply here; the 10% bound still rejects a stalled start.
                if (result.seconds > 0.0 &&
                    result.start_skew_seconds > 0.10 * result.seconds) {
                    throw std::runtime_error(
                        std::string("stress start skew above 10% for ") +
                        spec.name);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }
}

void run_latency_rounds(std::size_t samples, std::size_t warmup, int rounds) {
    for (int round = 0; round < rounds; ++round) {
        const auto& order = kOrders[static_cast<std::size_t>(round) % 6];
        for (int position = 0; position < 3; ++position) {
            const auto index = static_cast<std::size_t>(order[position]);
            const auto& spec = kLatencyQueues[index];
            const auto result = spec.run(samples, warmup);
            print_latency_trial(spec, round, samples, result);
            if (!result.ok) {
                throw std::runtime_error(
                    std::string("latency validation failed for ") + spec.name);
            }
        }
    }
}

double sentinel_sample() {
    std::uint64_t value = 0x243f6a8885a308d3ULL;
    const auto start = clock_type::now();
    for (std::uint64_t i = 0; i < kSentinelIterations; ++i) {
        value ^= value << 13U;
        value ^= value >> 7U;
        value ^= value << 17U;
    }
    volatile std::uint64_t sink = value;
    (void)sink;
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

double sentinel_baseline() {
    std::vector<double> samples;
    samples.reserve(5);
    for (int i = 0; i < 5; ++i) {
        samples.push_back(sentinel_sample());
    }
    const auto median = median_of(samples);
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto q1 = sorted[1];
    const auto q3 = sorted[3];
    if (median <= 0.0 || (q3 - q1) / median > 0.05) {
        throw std::runtime_error(
            "host is not quiet enough for the sentinel baseline");
    }
    return median;
}

workload_table calibrate_workloads(bool quick, std::uint64_t fixed_items) {
    workload_table workloads{};
    for (std::size_t queue = 0; queue < kThroughputQueues.size(); ++queue) {
        const auto& spec = kThroughputQueues[queue];
        for (std::size_t topo_index = 0; topo_index < kAllTopologies.size();
             ++topo_index) {
            const auto& topo = kAllTopologies[topo_index];
            if (quick || fixed_items != 0) {
                workloads[queue][topo_index] = quick ? 1'048'576 : fixed_items;
                continue;
            }
            std::vector<double> rates;
            rates.reserve(3);
            for (int i = 0; i < 3; ++i) {
                const auto result = spec.run(
                    topo.producers, topo.consumers, kCalibrationPilotItems,
                    warmup_items(kCalibrationPilotItems, topo.producers));
                if (!result.values_ok) {
                    throw std::runtime_error(
                        std::string("calibration validation failed for ") +
                        spec.name);
                }
                rates.push_back(result.rate_mops);
            }
            const auto rate = median_of(rates);
            if (rate <= 0.0) {
                throw std::runtime_error(
                    std::string("calibration produced no valid rate for ") +
                    spec.name);
            }
            // A 1.2 s target leaves margin over the 250 ms acceptance
            // floor after warm-up.  Each library and topology has its own
            // item count so a slow queue does not stretch a trial into a
            // thermal test.
            auto target =
                static_cast<std::uint64_t>(std::ceil(rate * 1.0e6 * 1.20));
            target = std::max<std::uint64_t>(target, 1'048'576ULL);
            target = std::min<std::uint64_t>(target, kMaxTotalItems);
            target = ((target + 63ULL) / 64ULL) * 64ULL;
            workloads[queue][topo_index] = target;
        }
    }
    return workloads;
}

workload_table fixed_workloads(std::uint64_t items) {
    workload_table workloads{};
    for (auto& queue : workloads) {
        queue.fill(items);
    }
    return workloads;
}

template<class Q> void verify_capacity(const char* name, std::size_t capacity) {
    auto queue = make_queue<Q>(capacity);
    if constexpr (requires { queue->is_lock_free(); }) {
        if (!queue->is_lock_free()) {
            throw std::runtime_error(std::string(name) +
                                     " is not lock-free on this target");
        }
    }
    for (std::size_t i = 0; i < capacity; ++i) {
        if (!queue_push(*queue, static_cast<value_type>(i))) {
            throw std::runtime_error(std::string(name) +
                                     " accepted fewer than capacity items");
        }
    }
    value_type value = 0;
    if (queue_push(*queue, static_cast<value_type>(capacity))) {
        throw std::runtime_error(std::string(name) +
                                 " accepted more than capacity items");
    }
    for (std::size_t i = 0; i < capacity; ++i) {
        if (!queue_pop(*queue, value) || value != static_cast<value_type>(i)) {
            throw std::runtime_error(std::string(name) +
                                     " did not preserve sequential FIFO order");
        }
    }
    if (queue_pop(*queue, value)) {
        throw std::runtime_error(std::string(name) + " did not drain empty");
    }
    std::fprintf(stderr, "capacity check: %s usable=%zu\n", name, capacity);
}

void verify_queues() {
    verify_capacity<ours_queue<kThroughputCapacity>>("mpmc-16B",
                                                     kThroughputCapacity);
    verify_capacity<rig_queue<kThroughputCapacity>>("rigtorp",
                                                    kThroughputCapacity);
    verify_capacity<boost_queue<kThroughputCapacity>>("boost",
                                                      kThroughputCapacity);
}

double clock_tick_ns() {
    const auto start = clock_type::now();
    auto next = start;
    for (int i = 0; i < 1'000'000 && next == start; ++i) {
        next = clock_type::now();
    }
    return std::chrono::duration<double, std::nano>(next - start).count();
}

void print_metadata(bool quick, const workload_table& workloads, int rounds,
                    std::size_t latency_samples, std::size_t latency_warmup,
                    double clock_tick, double max_load, double load1) {
    std::uint64_t largest_workload = 0;
    for (const auto& queue : workloads) {
        for (auto items : queue) {
            largest_workload = std::max(largest_workload, items);
        }
    }
    std::printf("{\"kind\":\"metadata\",\"schema_version\":3,"
                "\"profile_id\":\"comparison-v3\",\"mpmc_version\":\"%s\","
                "\"boost_version\":\"%s\",\"compiler\":\"%s\","
                "\"hardware_threads\":%u,\"placement\":\"%s\","
                "\"load1_start\":%.3f,\"max_load\":%.3f,"
                "\"throughput_capacity\":%zu,\"latency_capacity\":%zu,"
                "\"total_items\":%llu,\"workload_items\":[",
                mpmc::version_string, BOOST_LIB_VERSION,
                json_escape(__VERSION__).c_str(),
                std::max(1U, std::thread::hardware_concurrency()),
                json_escape(placement_description()).c_str(), load1, max_load,
                kThroughputCapacity, kLatencyCapacity,
                static_cast<unsigned long long>(largest_workload));
    for (std::size_t queue = 0; queue < workloads.size(); ++queue) {
        if (queue != 0) {
            std::printf(",");
        }
        std::printf("[");
        for (std::size_t topo = 0; topo < workloads[queue].size(); ++topo) {
            if (topo != 0) {
                std::printf(",");
            }
            std::printf("%llu", static_cast<unsigned long long>(
                                    workloads[queue][topo]));
        }
        std::printf("]");
    }
    std::printf("],\"rounds\":%d,\"latency_samples\":%zu,"
                "\"latency_warmup\":%zu,\"clock_tick_ns\":%.3f,"
                "\"seed\":%llu,\"quick\":%s}\n",
                rounds, latency_samples, latency_warmup, clock_tick,
                static_cast<unsigned long long>(kRngSeed),
                quick ? "true" : "false");
    std::printf(
        "{\"kind\":\"excluded\",\"library\":\"erez-16B\","
        "\"reason\":\"pinned upstream revision "
        "aef66f268f2b32143eb93ef857529025349c1f48 reads a sequence field "
        "non-atomically while other threads atomically replace the whole "
        "cell; excluded from the validated ranking\"}\n");
}

void print_host_record() {
    std::printf("{\"kind\":\"host\",\"load1_peak_primary\":%.3f,"
                "\"load1_peak_stress\":%.3f,\"load1_end\":%.3f}\n",
                g_peak_primary_load, g_peak_stress_load, load_average_1m());
}

bool parse_u64(const char* text, std::uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_size(const char* text, std::size_t& value) {
    std::uint64_t parsed = 0;
    if (!parse_u64(text, parsed) ||
        parsed > static_cast<std::uint64_t>(
                     std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_double(const char* text, double& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno == ERANGE ||
        !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t items = 0;
        std::size_t latency_samples = kDefaultLatencySamples;
        double max_load = 0.0;
        bool quick = false;
        bool allow_busy_host = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--quick") == 0) {
                quick = true;
            } else if (std::strcmp(argv[i], "--allow-busy-host") == 0) {
                allow_busy_host = true;
            } else if (std::strcmp(argv[i], "--items") == 0) {
                if (i + 1 >= argc) {
                    std::fprintf(stderr, "error: --items requires a value\n");
                    return 2;
                }
                if (!parse_u64(argv[++i], items)) {
                    std::fprintf(stderr,
                                 "error: --items requires a non-negative "
                                 "integer\n");
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--latency-samples") == 0) {
                if (i + 1 >= argc) {
                    std::fprintf(stderr,
                                 "error: --latency-samples requires a value\n");
                    return 2;
                }
                if (!parse_size(argv[++i], latency_samples)) {
                    std::fprintf(stderr, "error: --latency-samples requires a "
                                         "non-negative integer\n");
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--max-load") == 0) {
                if (i + 1 >= argc) {
                    std::fprintf(stderr,
                                 "error: --max-load requires a value\n");
                    return 2;
                }
                if (!parse_double(argv[++i], max_load)) {
                    std::fprintf(stderr,
                                 "error: --max-load requires a number\n");
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--help") == 0) {
                std::printf(
                    "usage: %s [--quick] [--items N] [--latency-samples N] "
                    "[--max-load X] [--allow-busy-host]\n",
                    argv[0]);
                return 0;
            } else {
                std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
                return 2;
            }
        }
        if (quick) {
            items = 1'048'576;
            latency_samples = 50'000;
        }
        if (latency_samples < 100 || latency_samples > 10'000'000) {
            std::fprintf(stderr,
                         "error: latency-samples must be in [100, 10000000]\n");
            return 2;
        }
        if (items != 0 && (items > kMaxTotalItems || items % 64 != 0)) {
            std::fprintf(stderr,
                         "error: items must be zero (auto-calibrate) or a "
                         "multiple of 64 no greater than %llu\n",
                         static_cast<unsigned long long>(kMaxTotalItems));
            return 2;
        }

        const unsigned hardware_threads =
            std::max(1U, std::thread::hardware_concurrency());
        if (max_load <= 0.0) {
            max_load =
                std::max(4.0, 0.5 * static_cast<double>(hardware_threads));
        }
        const double load1 = load_average_1m();
        if (!quick && !allow_busy_host && load1 >= 0.0 && load1 > max_load) {
            std::fprintf(stderr,
                         "error: host load average %.2f exceeds the limit "
                         "%.2f; wait for an idle host or pass "
                         "--allow-busy-host\n",
                         load1, max_load);
            return 2;
        }
        verify_queues();

        double sentinel = 0.0;
        if (!quick) {
            run_exact_validation();
        }
        const auto workloads =
            quick ? fixed_workloads(1'048'576)
                  : (items != 0 ? fixed_workloads(items)
                                : calibrate_workloads(false, 0));
        for (const auto& queue : workloads) {
            for (auto total_items : queue) {
                if (total_items > kMaxTotalItems || total_items % 64 != 0 ||
                    total_items % 8 != 0) {
                    throw std::runtime_error(
                        "invalid calibrated workload size");
                }
            }
        }
        if (!quick) {
            sentinel = sentinel_baseline();
            std::fprintf(stderr, "sentinel baseline: %.6f s\n", sentinel);
        }
        const int rounds = quick ? 1 : kPrimaryRounds;
        std::fprintf(stderr,
                     "workloads calibrated per library/topology; "
                     "%d rounds\n",
                     rounds);
        const auto latency_warmup = quick ? 10'000 : kLatencyWarmup;
        const auto clock_tick = clock_tick_ns();

        print_metadata(quick, workloads, rounds, latency_samples,
                       latency_warmup, clock_tick, max_load, load1);
        run_throughput_rounds(workloads, rounds, quick, sentinel);
        if (!apply_thread_placement(0)) {
            report_placement_failure();
        }
        run_latency_rounds(latency_samples, latency_warmup, rounds);

        const int failed_placements =
            placement_failures.load(std::memory_order_relaxed);
        if (failed_placements != 0) {
            if (quick) {
                std::fprintf(stderr,
                             "warning: thread placement failed for %d "
                             "threads; diagnostic result only\n",
                             failed_placements);
            } else {
                throw std::runtime_error(
                    "thread placement failed; no reference result published");
            }
        }
        print_host_record();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "error: unknown exception\n");
        return 1;
    }
}
