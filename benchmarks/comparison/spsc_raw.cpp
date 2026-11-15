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
// Single-producer/single-consumer raw throughput profile.
//
// This profile is intentionally separate from the general MPMC comparison:
// it measures one producer and one consumer on an empty queue with strict FIFO
// validation and no per-item bitmap.  A single consumer can validate every
// value against its expected sequence number, which catches loss, duplication,
// and reordering without adding a bitmap write to the hot path.
#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <boost/lockfree/spsc_queue.hpp>
#include <boost/version.hpp>
#include <mpmc/mpmc_queue.hpp>
#include <mpmc/spsc_queue.hpp>
#include <mpmc/version.hpp>
#include <rigtorp/MPMCQueue.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using value_type = std::uint64_t;
using clock_type = std::chrono::steady_clock;

constexpr std::size_t kCapacity = 4096;
constexpr std::uint64_t kDefaultItems = 150'000'000;
constexpr int kDefaultRounds = 18;
constexpr std::uint64_t kMaxItems = 1'000'000'000;

inline void spin_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    // NOLINTNEXTLINE(portability-no-assembler)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

template<class Q> bool queue_push(Q& queue, value_type value) {
    if constexpr (requires { queue.try_push(value); }) {
        return queue.try_push(value);
    } else {
        return queue.push(value);
    }
}

template<class Q> bool queue_pop(Q& queue, value_type& value) {
    if constexpr (requires { queue.try_pop(value); }) {
        return queue.try_pop(value);
    } else {
        return queue.pop(value);
    }
}

template<std::size_t Capacity>
using ours_queue = mpmc::spsc_queue<value_type, Capacity, std::uint32_t>;
template<std::size_t Capacity>
using boost_queue =
    boost::lockfree::spsc_queue<value_type,
                                boost::lockfree::capacity<Capacity>>;

template<class Q> struct queue_maker {
    static std::unique_ptr<Q> make() {
        return std::make_unique<Q>();
    }
};

struct trial {
    double seconds{0.0};
    double mops{0.0};
    bool ok{false};
    const char* status{"ok"};
};

template<class Q>
trial run_once(Q& queue, std::uint64_t items, std::uint64_t warmup) {
    trial result;
    std::atomic<bool> go{false};
    std::atomic<bool> valid{true};
    std::atomic<bool> aborted{false};

    // Warm the same queue and the same slot set before the timed run.  The
    // warm-up is executed by the caller's thread and is not timed.
    std::uint64_t value = 0;
    const auto warm_items = std::min<std::uint64_t>(warmup, kCapacity);
    const auto warm_deadline = clock_type::now() + std::chrono::seconds(120);
    unsigned warm_failures = 0;
    for (std::uint64_t i = 0; i < warm_items; ++i) {
        while (!queue_push(queue, i)) {
            if ((++warm_failures & 63U) == 0U &&
                clock_type::now() > warm_deadline) {
                result.ok = false;
                result.status = "timeout";
                return result;
            }
            spin_relax();
        }
    }
    for (std::uint64_t i = 0; i < warm_items; ++i) {
        while (!queue_pop(queue, value)) {
            if ((++warm_failures & 63U) == 0U &&
                clock_type::now() > warm_deadline) {
                result.ok = false;
                result.status = "timeout";
                return result;
            }
            spin_relax();
        }
        if (value != i) {
            valid.store(false, std::memory_order_relaxed);
        }
    }

    const auto deadline = clock_type::now() + std::chrono::seconds(120);
    std::thread producer([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        unsigned push_failures = 0;
        for (std::uint64_t i = 0; i < items; ++i) {
            while (!queue_push(queue, i)) {
                if (aborted.load(std::memory_order_relaxed) ||
                    (((++push_failures) & 63U) == 0U &&
                     clock_type::now() > deadline)) {
                    aborted.store(true, std::memory_order_relaxed);
                    return;
                }
                spin_relax();
            }
            if (aborted.load(std::memory_order_relaxed)) {
                return;
            }
        }
    });
    std::thread consumer([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t expected = 0;
        unsigned pop_failures = 0;
        while (expected < items) {
            value = 0;
            if (queue_pop(queue, value)) {
                if (value != expected) {
                    valid.store(false, std::memory_order_relaxed);
                    aborted.store(true, std::memory_order_relaxed);
                    return;
                }
                ++expected;
            } else if (aborted.load(std::memory_order_relaxed) ||
                       (((++pop_failures) & 63U) == 0U &&
                        clock_type::now() > deadline)) {
                aborted.store(true, std::memory_order_relaxed);
                return;
            } else {
                spin_relax();
            }
        }
    });

    const auto start = clock_type::now();
    go.store(true, std::memory_order_release);
    producer.join();
    consumer.join();
    const auto elapsed =
        std::chrono::duration<double>(clock_type::now() - start).count();
    result.seconds = elapsed;
    result.mops = static_cast<double>(items) / elapsed / 1'000'000.0;
    if (aborted.load(std::memory_order_relaxed)) {
        result.ok = false;
        result.status = "timeout";
    } else {
        std::uint64_t trailing = 0;
        if (queue_pop(queue, trailing)) {
            result.ok = false;
            result.status = "mismatch";
        } else {
            result.ok = valid.load(std::memory_order_relaxed);
            result.status = result.ok ? "ok" : "mismatch";
        }
    }
    return result;
}
template<class Q>
void run_round(const char* name, int round, std::uint64_t items,
               std::uint64_t warmup) {
    auto queue = queue_maker<Q>::make();
    const trial result = run_once(*queue, items, warmup);
    std::printf("{\"kind\":\"spsc\",\"library\":\"%s\",\"round\":%d,"
                "\"items\":%llu,\"seconds\":%.9f,\"Mitems_s\":%.6f,"
                "\"validation\":\"%s\"}\n",
                name, round, static_cast<unsigned long long>(items),
                result.seconds, result.mops, result.status);
    if (!result.ok) {
        throw std::runtime_error(std::string("FIFO validation failed for ") +
                                 name);
    }
}

struct spec {
    const char* name;
    void (*run)(int, std::uint64_t, std::uint64_t);
};

constexpr std::array<spec, 2> kQueues{{
    {"mpmc-spsc",
     [](int round, std::uint64_t items, std::uint64_t warmup) {
         run_round<ours_queue<kCapacity>>("mpmc-spsc", round, items, warmup);
     }},
    {"boost-spsc",
     [](int round, std::uint64_t items, std::uint64_t warmup) {
         run_round<boost_queue<kCapacity>>("boost-spsc", round, items, warmup);
     }},
}};

bool parse_u64(const char* text, std::uint64_t& value) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    const char* end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::uint64_t items = kDefaultItems;
        int rounds = kDefaultRounds;
        bool quick = false;
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--quick") == 0) {
                quick = true;
            } else if (std::strcmp(argv[i], "--items") == 0) {
                if (i + 1 >= argc) {
                    std::fprintf(stderr,
                                 "error: --items requires an integer\n");
                    return 2;
                }
                const char* value = argv[++i];
                if (!parse_u64(value, items)) {
                    std::fprintf(stderr,
                                 "error: --items requires an integer\n");
                    return 2;
                }
            } else if (std::strcmp(argv[i], "--rounds") == 0) {
                if (i + 1 >= argc) {
                    std::fprintf(stderr,
                                 "error: --rounds requires an integer\n");
                    return 2;
                }
                const char* value = argv[++i];
                std::uint64_t parsed_rounds = 0;
                if (!parse_u64(value, parsed_rounds)) {
                    std::fprintf(stderr,
                                 "error: --rounds requires an integer\n");
                    return 2;
                }
                if (parsed_rounds == 0 || parsed_rounds > 10'000) {
                    std::fprintf(stderr,
                                 "error: rounds must be in [1, 10000]\n");
                    return 2;
                }
                rounds = static_cast<int>(parsed_rounds);
            } else if (std::strcmp(argv[i], "--help") == 0) {
                std::printf("usage: %s [--quick] [--items N] [--rounds N]\n",
                            argv[0]);
                return 0;
            } else {
                std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
                return 2;
            }
        }
        if (quick) {
            items = 5'000'000;
            rounds = 2;
        }
        if (items == 0 || items > kMaxItems) {
            std::fprintf(stderr, "error: items must be in [1, %llu]\n",
                         static_cast<unsigned long long>(kMaxItems));
            return 2;
        }
        const auto warmup = std::min<std::uint64_t>(items, 65'536ULL);
        const auto warm_items = std::min<std::uint64_t>(warmup, kCapacity);
        std::printf("{\"kind\":\"spsc-metadata\",\"profile\":\"spsc-raw\","
                    "\"mpmc_version\":\"%s\",\"boost_version\":\"%s\","
                    "\"compiler\":\"%s\",\"hardware_threads\":%u,"
                    "\"capacity\":%zu,\"items\":%llu,\"rounds\":%d,"
                    "\"warmup\":%llu,\"quick\":%s}\n",
                    mpmc::version_string, BOOST_LIB_VERSION, __VERSION__,
                    std::max(1U, std::thread::hardware_concurrency()),
                    kCapacity, static_cast<unsigned long long>(items), rounds,
                    static_cast<unsigned long long>(warm_items),
                    quick ? "true" : "false");
        for (int round = 0; round < rounds; ++round) {
            const int offset = round % static_cast<int>(kQueues.size());
            for (int position = 0; position < static_cast<int>(kQueues.size());
                 ++position) {
                const auto& queue =
                    kQueues[static_cast<std::size_t>(offset + position) %
                            kQueues.size()];
                queue.run(round, items, warmup);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "error: %s\n", error.what());
        return 1;
    }
}
