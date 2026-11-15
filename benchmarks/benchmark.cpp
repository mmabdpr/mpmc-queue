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
// Micro-benchmark for mpmc_queue.
//
// Two measurements are reported:
//
//  * transfer throughput: producers push and consumers pop a fixed number of
//    items after a coordinated start.  Throughput counts *transferred items*
//    (one successful push and one successful pop per item), not individual
//    queue operations.  Each configuration is warmed up once, then repeated;
//    the mean, minimum, maximum and relative spread over the repetitions are
//    printed so run-to-run variability is visible.
//
//  * ping-pong round-trip latency: one thread sends an item and waits for it
//    to come back.  The printed figures are round trips (two queue transfers
//    plus two thread wake-ups), not one-way latency.
//
// The configurations cross a small part of the design space: 8- and 16-byte
// atomic cells, eager and lazy push/pop, two capacities, and 1/2/4
// producer/consumer pairs.  They are a cross-section, not a full sweep; use
// --items, --reps and --quick to trade accuracy for runtime.
//
// Hot-path counting is per consumer and is aggregated only after join, so no
// shared atomic counter is touched per pop.  The only shared atomic read on
// the consumer path is the "producers finished" flag, and it is read only
// when a pop attempt fails (the queue looks empty).

#include <mpmc/mpmc.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

/// Effective 16-byte-cell capability for every selection in this benchmark.
/// CMake folds MPMC_FORCE_8_BYTE_CELLS into
/// MPMC_BENCHMARK_HAS_16BYTE_CELL, so a forced-8 build must report `false`
/// here even when 16-byte cells are lock-free on the target.  Requiring the
/// compiler probe as well keeps a stale or inconsistent CMake cache from
/// instantiating a cell that the compiler will reject.
inline constexpr bool has_16_byte_cells =
#if defined(MPMC_BENCHMARK_HAS_16BYTE_CELL)
    MPMC_BENCHMARK_HAS_16BYTE_CELL != 0 &&
#endif
    mpmc::detail::has_lock_free_16_byte_cell;

/// Ping-pong exchanges performed before the timed samples.  The request and
/// response queues are constructed once, so the warm-up and the samples share
/// one sequence generation.
inline constexpr std::uint64_t latency_warmup = 1000;

/// Sizes of the timed and latency queues.  The publication-limit checks below
/// use the largest sweep capacity.
inline constexpr std::size_t max_sweep_capacity = 4096;
inline constexpr std::size_t latency_capacity = 2;

/// Every benchmark queue uses a 32-bit index, so the publication horizon is a
/// property of the index type alone; the payload and cell size do not change
/// it.  Use an 8-byte payload here so this helper stays instantiable on
/// 8-byte-only targets.
template<class Index>
[[nodiscard]] constexpr std::uint64_t benchmark_marker_horizon() noexcept {
    using queue_type = mpmc::mpmc_queue<std::uint32_t, 0, Index>;
    return static_cast<std::uint64_t>(queue_type::marker_horizon);
}

/// Largest workload one throughput queue generation can publish.
inline constexpr std::uint64_t max_throughput_items =
    benchmark_marker_horizon<std::uint32_t>() - max_sweep_capacity;

/// Largest number of warm-up + measured ping-pong exchanges one generation can
/// publish.  Both the request and the response queue are used once per
/// exchange, and both start at position zero.
inline constexpr std::uint64_t latency_publication_limit =
    benchmark_marker_horizon<std::uint32_t>() - latency_capacity;

/// Largest accepted --latency-samples value: the warm-up exchanges share the
/// same queue generation.
inline constexpr std::uint64_t max_latency_samples =
    latency_publication_limit - latency_warmup;

static_assert(max_throughput_items > 0);
static_assert(max_throughput_items >= 1000);
static_assert(latency_publication_limit > latency_warmup);

/// Rows emitted by the sweep, audited by cell size so the self-check can prove
/// that a disabled 16-byte cell produced no measurement rows.
struct row_audit {
    unsigned rows_8b = 0;
    unsigned rows_16b = 0;
};

struct options {
    std::uint64_t items = 500'000;
    std::uint32_t reps = 3;
    std::uint64_t latency_samples = 50'000;
    bool check = false;
};

double elapsed_seconds(clock_type::time_point start) {
    return std::chrono::duration<double>(clock_type::now() - start).count();
}

/// A start gate so thread creation and scheduling do not leak into the timed
/// window.  Every participant arrives, then the main thread records the start
/// time and releases all participants together.
class start_gate {
public:
    explicit start_gate(std::uint32_t participants) noexcept
        : participants_(participants) {}

    void wait_for_start() noexcept {
        ready_.fetch_add(1, std::memory_order_acq_rel);
        while (!go_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void wait_until_ready() const noexcept {
        while (ready_.load(std::memory_order_acquire) != participants_) {
            std::this_thread::yield();
        }
    }

    void release() noexcept {
        go_.store(true, std::memory_order_release);
    }

private:
    const std::uint32_t participants_;
    std::atomic<std::uint32_t> ready_{0};
    std::atomic<bool> go_{false};
};

template<class Payload>
Payload make_payload(std::uint32_t producer, std::uint64_t index) noexcept {
    return static_cast<Payload>((static_cast<std::uint64_t>(producer) << 32U) |
                                (index & 0xffff'ffffULL));
}

struct run_result {
    double seconds;
    std::uint64_t transferred;
};

template<class Payload, class Index, bool LazyPush, bool LazyPop>
run_result run_once(std::size_t capacity, std::uint32_t producers,
                    std::uint32_t consumers, std::uint64_t items) {
    using queue_type = mpmc::mpmc_queue<Payload, 0, Index, LazyPush, LazyPop>;
    auto queue = queue_type::create(capacity);
    if (!queue) {
        std::fprintf(stderr, "error: could not create a %zu-slot queue\n",
                     capacity);
        std::exit(1); // NOLINT(concurrency-mt-unsafe)
    }

    const std::uint64_t base = items / producers;
    const std::uint64_t extra = items % producers;

    start_gate gate{producers + consumers};
    std::atomic<std::uint32_t> producers_done{0};
    std::atomic<bool> exhausted{false};
    std::vector<std::uint64_t> counts(consumers, 0);
    std::vector<std::thread> threads;
    threads.reserve(producers + consumers);

    for (std::uint32_t p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            const std::uint64_t begin = static_cast<std::uint64_t>(p) * base +
                                        std::min<std::uint64_t>(p, extra);
            const std::uint64_t end =
                begin + base + (static_cast<std::uint64_t>(p) < extra ? 1 : 0);
            gate.wait_for_start();
            for (std::uint64_t i = begin; i < end; ++i) {
                const Payload value = make_payload<Payload>(p, i);
                while (!queue->try_push(value)) {
                    if (queue->sequence_exhausted()) {
                        // The publication horizon was reached: no thread can
                        // publish again, so stop instead of retrying forever.
                        exhausted.store(true, std::memory_order_release);
                        break;
                    }
                    std::this_thread::yield();
                }
                if (exhausted.load(std::memory_order_acquire)) {
                    break;
                }
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    }

    for (std::uint32_t c = 0; c < consumers; ++c) {
        threads.emplace_back([&, c] {
            // Local, register-resident count.  It is written once, after the
            // loop, so consumers never contend on the same cache line.
            std::uint64_t count = 0;
            Payload value{};
            gate.wait_for_start();
            for (;;) {
                if (queue->try_pop(value)) {
                    ++count;
                } else if (producers_done.load(std::memory_order_acquire) ==
                           producers) {
                    // All pushes are visible after observing every producer's
                    // release increment.  One last attempt distinguishes
                    // "drained" from "the queue was momentarily empty".
                    if (queue->try_pop(value)) {
                        ++count;
                    } else {
                        break;
                    }
                } else if (exhausted.load(std::memory_order_acquire)) {
                    // A producer stopped at the publication horizon.  The run
                    // cannot transfer the requested item count and reports an
                    // error; do not spin while it unwinds.
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
            counts[c] = count;
        });
    }

    gate.wait_until_ready();
    const auto start = clock_type::now();
    gate.release();
    for (std::thread& thread : threads) {
        thread.join();
    }
    const double seconds = elapsed_seconds(start);

    if (exhausted.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
                     "error: a %zu-slot queue reached its publication limit "
                     "before transferring %llu items; the workload exceeds the "
                     "32-bit index sequence capacity\n",
                     capacity, static_cast<unsigned long long>(items));
        std::exit(1); // NOLINT(concurrency-mt-unsafe)
    }

    std::uint64_t transferred = 0;
    for (std::uint64_t count : counts) {
        transferred += count;
    }
    return {seconds, transferred};
}

struct stats {
    double mean = 0.0;
    double min = std::numeric_limits<double>::max();
    double max = 0.0;
    double spread_percent = 0.0;
};

stats summarize(const std::vector<double>& values) {
    stats result;
    if (values.empty()) {
        return result;
    }
    double sum = 0.0;
    for (double value : values) {
        sum += value;
        result.min = std::min(result.min, value);
        result.max = std::max(result.max, value);
    }
    result.mean = sum / static_cast<double>(values.size());
    if (result.mean > 0.0) {
        result.spread_percent = (result.max - result.min) / result.mean * 100.0;
    }
    return result;
}

template<class Payload, class Index, bool LazyPush, bool LazyPop>
void run_configuration(const char* cell_label, const char* mode_label,
                       std::size_t capacity, std::uint32_t producers,
                       std::uint32_t consumers, const options& opt,
                       row_audit& audit) {
    // Warm up code paths, thread stacks and the allocator before timing.  The
    // warm-up uses the same topology and queue type but a quarter of the
    // items, and its result is discarded.
    const std::uint64_t warmup_items =
        std::max<std::uint64_t>(opt.items / 4, 1000);
    (void)run_once<Payload, Index, LazyPush, LazyPop>(capacity, producers,
                                                      consumers, warmup_items);

    std::vector<double> m_items_per_second;
    m_items_per_second.reserve(opt.reps);
    std::uint64_t mismatched_runs = 0;

    for (std::uint32_t rep = 0; rep < opt.reps; ++rep) {
        const run_result result = run_once<Payload, Index, LazyPush, LazyPop>(
            capacity, producers, consumers, opt.items);
        if (result.transferred != opt.items) {
            ++mismatched_runs;
        }
        if (result.seconds > 0.0) {
            m_items_per_second.push_back(
                static_cast<double>(result.transferred) / result.seconds /
                1.0e6);
        }
    }

    const stats summary = summarize(m_items_per_second);
    if (std::strcmp(cell_label, "16B") == 0) {
        ++audit.rows_16b;
    } else {
        ++audit.rows_8b;
    }
    std::printf("%-6s %-10s cap=%-5zu %2uP/%2uC  %9.2f %9.2f %9.2f %7.1f%%\n",
                cell_label, mode_label, capacity, producers, consumers,
                summary.mean, summary.min, summary.max, summary.spread_percent);

    if (mismatched_runs != 0) {
        std::fprintf(stderr,
                     "warning: %s %s cap=%zu %uP/%uC transferred the wrong "
                     "number of items in %llu of %u runs\n",
                     cell_label, mode_label, capacity, producers, consumers,
                     static_cast<unsigned long long>(mismatched_runs),
                     static_cast<unsigned int>(opt.reps));
    }
}

template<class Payload, class Index>
void run_cell(const char* cell_label, const options& opt, row_audit& audit) {
    constexpr std::size_t capacities[] = {256, max_sweep_capacity};
    constexpr std::uint32_t topologies[][2] = {{1, 1}, {2, 2}, {4, 4}};

    for (std::size_t capacity : capacities) {
        for (const auto& topology : topologies) {
            const std::uint32_t producers = topology[0];
            const std::uint32_t consumers = topology[1];
            run_configuration<Payload, Index, false, false>(
                cell_label, "eager", capacity, producers, consumers, opt,
                audit);
        }
    }
    for (std::size_t capacity : capacities) {
        for (const auto& topology : topologies) {
            const std::uint32_t producers = topology[0];
            const std::uint32_t consumers = topology[1];
            run_configuration<Payload, Index, true, false>(
                cell_label, "lazy-push", capacity, producers, consumers, opt,
                audit);
        }
    }
    for (std::size_t capacity : capacities) {
        for (const auto& topology : topologies) {
            const std::uint32_t producers = topology[0];
            const std::uint32_t consumers = topology[1];
            run_configuration<Payload, Index, false, true>(
                cell_label, "lazy-pop", capacity, producers, consumers, opt,
                audit);
        }
    }
    for (std::size_t capacity : capacities) {
        for (const auto& topology : topologies) {
            const std::uint32_t producers = topology[0];
            const std::uint32_t consumers = topology[1];
            run_configuration<Payload, Index, true, true>(
                cell_label, "lazy-both", capacity, producers, consumers, opt,
                audit);
        }
    }
}

template<class Payload, class Index>
[[nodiscard]] bool ping_pong_latency(std::uint64_t samples) {
    using queue_type = mpmc::mpmc_queue<Payload, latency_capacity, Index>;
    queue_type request;
    queue_type response;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};

    std::thread echo([&] {
        while (!stop.load(std::memory_order_acquire)) {
            Payload value{};
            if (!request.try_pop(value)) {
                continue;
            }
            while (!response.try_push(value)) {
                if (response.sequence_exhausted()) {
                    // No response publication position remains; retrying
                    // cannot make progress and would spin forever.
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
            }
        }
    });

    const auto exchange = [&](std::uint64_t& latency_ns) {
        const auto sent = clock_type::now();
        // The stamp is only a payload sentinel; truncation is harmless.
        const Payload stamp = static_cast<Payload>(
            static_cast<std::uint64_t>(sent.time_since_epoch().count()));
        while (!request.try_push(stamp)) {
            if (request.sequence_exhausted() ||
                failed.load(std::memory_order_acquire)) {
                return false;
            }
            std::this_thread::yield();
        }
        Payload echoed{};
        while (!response.try_pop(echoed)) {
            if (failed.load(std::memory_order_acquire)) {
                // The echo thread stopped at the response publication horizon.
                return false;
            }
        }
        const auto received = clock_type::now();
        latency_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(received -
                                                                 sent)
                .count());
        return true;
    };

    const auto abort_run = [&] {
        stop.store(true, std::memory_order_release);
        echo.join();
        std::fprintf(stderr, "error: ping-pong latency test reached the queue "
                             "publication limit before completing its samples; "
                             "reduce --latency-samples\n");
        return false;
    };

    std::uint64_t ignored = 0;
    for (std::uint64_t i = 0; i < latency_warmup; ++i) {
        if (!exchange(ignored)) {
            return abort_run();
        }
    }

    std::vector<std::uint64_t> latencies;
    latencies.reserve(static_cast<std::size_t>(samples));
    for (std::uint64_t i = 0; i < samples; ++i) {
        std::uint64_t latency = 0;
        if (!exchange(latency)) {
            return abort_run();
        }
        latencies.push_back(latency);
    }

    stop.store(true, std::memory_order_release);
    echo.join();

    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double p) {
        const std::size_t index = static_cast<std::size_t>(
            p * static_cast<double>(latencies.size() - 1));
        return latencies[index];
    };
    std::uint64_t sum = 0;
    for (std::uint64_t value : latencies) {
        sum += value;
    }
    std::printf(
        "\nping-pong round trip (1P/1C, %zu-byte cell, %llu samples):\n"
        "  mean %6.1f ns  p50 %4llu  p90 %4llu  p99 %4llu  p99.9 %4llu  "
        "max %4llu\n",
        queue_type::entry_size, static_cast<unsigned long long>(samples),
        static_cast<double>(sum) / static_cast<double>(latencies.size()),
        static_cast<unsigned long long>(percentile(0.50)),
        static_cast<unsigned long long>(percentile(0.90)),
        static_cast<unsigned long long>(percentile(0.99)),
        static_cast<unsigned long long>(percentile(0.999)),
        static_cast<unsigned long long>(latencies.back()));
    return true;
}

void print_usage(const char* program) {
    std::printf(
        "usage: %s [options]\n"
        "  --items N            items transferred per timed run (default "
        "500000)\n"
        "  --reps N             timed repetitions per configuration (default "
        "3)\n"
        "  --latency-samples N  ping-pong samples (default 50000)\n"
        "  --quick              short reduced-workload run\n"
        "  --check              short cell-selection self-check, then exit\n"
        "  --help               print this message\n",
        program);
}

/// Strict unsigned decimal parser: no sign, no whitespace, no trailing bytes.
/// std::from_chars reports out-of-range instead of silently saturating.
bool parse_u64(const char* text, std::uint64_t& out) {
    if (text == nullptr || *text == '\0') {
        return false;
    }
    if (*text == '+' || *text == '-') {
        return false;
    }
    const char* const end = text + std::strlen(text);
    std::uint64_t value = 0;
    const std::from_chars_result result = std::from_chars(text, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

[[noreturn]] void missing_value(const char* program, const char* option) {
    std::fprintf(stderr, "error: %s requires a value\n", option);
    print_usage(program);
    std::exit(2); // NOLINT(concurrency-mt-unsafe)
}

[[noreturn]] void invalid_value(const char* program, const char* option,
                                const char* value, const char* expected) {
    std::fprintf(stderr, "error: %s expects %s, got '%s'\n", option, expected,
                 value);
    print_usage(program);
    std::exit(2); // NOLINT(concurrency-mt-unsafe)
}

/// Returns false when the process should exit successfully without running.
bool parse_options(int argc, char** argv, options& opt) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return false;
        }
        if (std::strcmp(arg, "--quick") == 0) {
            opt.items = 100'000;
            opt.reps = 1;
            opt.latency_samples = 5'000;
            continue;
        }
        if (std::strcmp(arg, "--check") == 0) {
            opt.check = true;
            continue;
        }

        const bool items_option = std::strcmp(arg, "--items") == 0;
        const bool reps_option = std::strcmp(arg, "--reps") == 0;
        const bool samples_option = std::strcmp(arg, "--latency-samples") == 0;
        if (!items_option && !reps_option && !samples_option) {
            std::fprintf(stderr, "error: invalid argument: %s\n", arg);
            print_usage(argv[0]);
            std::exit(2); // NOLINT(concurrency-mt-unsafe)
        }
        if (i + 1 >= argc) {
            missing_value(argv[0], arg);
        }

        const char* text = argv[++i];
        std::uint64_t value = 0;
        if (!parse_u64(text, value)) {
            invalid_value(argv[0], arg, text,
                          "a positive decimal integer without a sign");
        }

        if (items_option) {
            if (value == 0) {
                invalid_value(argv[0], arg, text,
                              "a positive decimal integer without a sign");
            }
            opt.items = value;
        } else if (reps_option) {
            if (value < 1 || value > 1000) {
                invalid_value(argv[0], arg, text, "an integer in [1, 1000]");
            }
            opt.reps = static_cast<std::uint32_t>(value);
        } else {
            if (value == 0) {
                invalid_value(argv[0], arg, text,
                              "a positive decimal integer without a sign");
            }
            opt.latency_samples = value;
        }
    }

    if (opt.check) {
        // Keep the self-check short and independent of flag ordering.
        opt.items = 2000;
        opt.reps = 1;
        opt.latency_samples = 200;
    }
    return true;
}

/// Reject workloads that a queue generation cannot encode.  Exceeding
/// publication_limit() does not fail one push; it permanently stops new
/// publications, so the retry loops would spin forever without making
/// progress.
[[nodiscard]] bool validate_options(const options& opt) {
    // The global warm-up uses half the timed item count; each per-configuration
    // warm-up uses a quarter.  Half is therefore the largest single-generation
    // throughput workload derived from --items.
    const std::uint64_t throughput_warmup =
        std::max<std::uint64_t>(opt.items / 2, 1000);
    if (opt.items > max_throughput_items ||
        throughput_warmup > max_throughput_items) {
        std::fprintf(stderr,
                     "error: --items %llu is too large: a single throughput "
                     "run (including its warm-up) must stay within the queue "
                     "publication limit of %llu publications\n",
                     static_cast<unsigned long long>(opt.items),
                     static_cast<unsigned long long>(max_throughput_items));
        return false;
    }

    // The ping-pong warm-up and the measured samples share one queue
    // generation, so validate their sum rather than the samples alone.
    if (opt.latency_samples > max_latency_samples) {
        std::fprintf(stderr,
                     "error: --latency-samples %llu is too large: the "
                     "warm-up plus samples must stay within the ping-pong "
                     "queue publication limit of %llu exchanges (at most %llu "
                     "samples)\n",
                     static_cast<unsigned long long>(opt.latency_samples),
                     static_cast<unsigned long long>(latency_publication_limit),
                     static_cast<unsigned long long>(max_latency_samples));
        return false;
    }

    // Each repetition constructs a fresh queue, so --reps does not consume
    // publication positions and needs no additional horizon check.
    return true;
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    options opt;
    if (!parse_options(argc, argv, opt)) {
        return 0;
    }
    if (!validate_options(opt)) {
        return 2;
    }

    using queue_8 = mpmc::mpmc_queue<std::uint32_t, 0, std::uint32_t>;
    static_assert(queue_8::entry_size == 8);

    std::printf("mpmc_queue %s benchmark\n", mpmc::version_string);
#if defined(__VERSION__)
    std::printf("  compiler: %s\n", __VERSION__);
#elif defined(_MSC_VER)
    std::printf("  compiler: MSVC %d\n", _MSC_VER);
#else
    std::printf("  compiler: unknown\n");
#endif
    std::printf("  hardware threads: %u\n",
                std::thread::hardware_concurrency());
    std::printf("  cell 8B:  payload=%zu index=%zu lock_free=%d enabled=%d\n",
                mpmc::detail::cell_payload_bytes_v<std::uint32_t, 8>,
                sizeof(std::uint32_t),
                mpmc::detail::has_lock_free_8_byte_cell ? 1 : 0, 1);
    std::printf("  cell 16B: payload=%zu index=%zu lock_free=%d enabled=%d\n",
                mpmc::detail::cell_payload_bytes_v<std::uint32_t, 16>,
                sizeof(std::uint32_t),
                mpmc::detail::has_lock_free_16_byte_cell ? 1 : 0,
                has_16_byte_cells ? 1 : 0);
    std::printf("  items/run=%llu reps=%u latency-samples=%llu\n",
                static_cast<unsigned long long>(opt.items), opt.reps,
                static_cast<unsigned long long>(opt.latency_samples));
    std::printf(
        "\ntransferred items/s (10^6): one item is one successful push and one "
        "successful pop;\nstart is coordinated, thread creation and warm-up "
        "are "
        "excluded, and each configuration\nis repeated; spread = "
        "(max-min)/mean. "
        "Not a statistical benchmark.\n\n");
    std::printf("%-6s %-10s %-9s %-8s %9s %9s %9s %7s\n", "cell", "mode",
                "capacity", "threads", "mean", "min", "max", "spread");

    // Global warm-up before the sweep: otherwise the first reported
    // configuration is measured while the cores are still ramping to a steady
    // clock.  Its result is discarded.
    (void)run_once<std::uint32_t, std::uint32_t, false, false>(
        max_sweep_capacity, 4, 4, std::max<std::uint64_t>(opt.items / 2, 1000));

    row_audit audit;
    run_cell<std::uint32_t, std::uint32_t>("8B", opt, audit);
    if constexpr (has_16_byte_cells) {
        run_cell<std::uint64_t, std::uint32_t>("16B", opt, audit);
    } else {
        std::printf("\n16-byte cells are not lock-free on this target or are "
                    "disabled by the build; skipping the 16-byte sweep and "
                    "using an 8-byte cell for the ping-pong test\n");
    }

    bool latency_ok = false;
    if constexpr (has_16_byte_cells) {
        latency_ok = ping_pong_latency<std::uint64_t, std::uint32_t>(
            opt.latency_samples);
    } else {
        latency_ok = ping_pong_latency<std::uint32_t, std::uint32_t>(
            opt.latency_samples);
    }
    if (!latency_ok) {
        return 1;
    }

    if (opt.check) {
        const unsigned expected_16 = has_16_byte_cells ? audit.rows_8b : 0;
        if (audit.rows_8b == 0 || audit.rows_16b != expected_16) {
            std::fprintf(stderr,
                         "error: benchmark cell-selection self-check failed: "
                         "8-byte rows=%u, 16-byte rows=%u (expected 16-byte "
                         "rows=%u)\n",
                         audit.rows_8b, audit.rows_16b, expected_16);
            return 1;
        }
        std::printf("\nbenchmark self-check: PASS; 8-byte rows=%u, "
                    "16-byte rows=%u\n",
                    audit.rows_8b, audit.rows_16b);
    }
    return 0;
}
