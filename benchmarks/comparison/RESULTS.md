# Cross-library benchmark results

Status: **reference run with diagnostics**. This
file is generated from `reference.json`; both artifacts
share the same profile and run metadata. Asymmetric
cases that do not meet the primary stability bounds are
reported as diagnostics rather than used for a ranking
claim.

## Environment

| Item | Value |
| --- | --- |
| Profile | `comparison-v3` |
| mpmc_queue | `1.0.0` |
| Boost.Lockfree | `1_92` |
| Compiler | `Apple LLVM 21.0.0 (clang-2100.4.1.1)` |
| CPU | Apple M3 Max |
| OS | `Darwin 27.2.0 (arm64)` |
| Hardware threads | 14 |
| Start load average | 7.94 |
| Host load limit | 7.00 |
| Primary load peak | 27.01 |
| Stress load peak | 22.70 |
| Clock tick | ~42.0 ns |
| Placement | macOS user-interactive QoS with taskpolicy latency tier; scheduler-managed, no hard CPU affinity |
| Throughput capacity | 4096 |
| Latency capacity | 64 |
| Primary rounds | 18 |
| Latency samples per round | 1000000 |

The load peak is sampled during the run and includes the
benchmark's own workers; the start load is the host gate.

The start load was above the default host limit; this reference run used
`--allow-busy-host`. The symmetric primary cases still met the stability
bounds, and the one unstable asymmetric case is marked diagnostic.

Boost.Lockfree and rigtorp use different internal metadata
layouts. mpmc-16B uses a 16-byte atomic cell with an 8-byte
payload and a 32-bit index. The comparison is of queue APIs
and behavior, not byte-for-byte identical storage.

Calibrated items per trial (millions), one value per
topology in the order 1P/1C, 2P/2C, 4P/4C, 4P/1C, 1P/4C,
8P/8C:

| Queue | 1P/1C | 2P/2C | 4P/4C | 4P/1C | 1P/4C | 8P/8C |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| mpmc-16B | 134.22 | 24.61 | 6.06 | 20.11 | 15.62 | 4.17 |
| rigtorp | 134.22 | 24.21 | 5.65 | 10.44 | 10.27 | 3.08 |
| boost | 9.89 | 6.73 | 2.90 | 5.33 | 7.92 | 1.32 |

## Primary throughput

Timed transfer throughput in millions of items per second,
median with interquartile range in parentheses. One item is
one successful enqueue and one successful dequeue. Timed
workers validate every value with count, wrapped 64-bit sum,
and wrapped sum-of-squares. A separate untimed pass checks
every library and topology item-by-item with an exact bitmap.

| Queue | 1P/1C | 2P/2C | 4P/4C | 4P/1C | 1P/4C |
| --- | ---: | ---: | ---: | ---: | ---: |
| mpmc-16B | 198.9 (198.0-199.4) | 21.1 (20.8-21.3) | 4.9 (4.8-5.0) | 17.2 (16.5-17.6) | 13.7 (13.5-13.8) |
| rigtorp | 202.8 (201.8-203.4) | 20.5 (19.6-21.0) | 5.0 (4.9-5.2) | 8.7 (8.6-8.7) | 8.5 (8.5-8.6) |
| boost | 8.2 (7.8-8.4) | 5.8 (5.8-6.0) | 2.5 (2.4-2.6) | 3.7 (3.7-4.6)* | 6.7 (6.6-6.8) |

* Diagnostic only: this asymmetric case did not meet
  the primary stability bounds on this host. The value
  is reported but is not used as a ranking claim.

Paired ratios use the same round for each comparison. A
difference is called clear only when the bootstrap 95%
interval lies wholly outside the 0.95-1.05 equivalence band.
The ratio is the median of paired per-round ratios, not the
ratio of the displayed medians; rounding can make the two
differ slightly.

| Queue | 1P/1C | 2P/2C | 4P/4C | 4P/1C | 1P/4C |
| --- | ---: | ---: | ---: | ---: | ---: |
| mpmc-16B | baseline | baseline | baseline | baseline | baseline |
| rigtorp | 1.02x (1.01-1.02), unresolved | 0.98x (0.94-0.99), unresolved | 1.05x (1.01-1.09), unresolved | 0.51x (0.50-0.52), slower | 0.62x (0.62-0.63), slower |
| boost | 0.04x (0.04-0.04), slower | 0.28x (0.27-0.28), slower | 0.51x (0.49-0.53), slower | 0.22x (0.21-0.26), diagnostic* | 0.49x (0.48-0.49), slower |

## 8P/8C scheduler-stress

The 8P/8C case runs sixteen workers on a 14-thread host and
is reported separately. It measures the queue plus scheduler
and cache contention; it is not a scaling claim.

| Queue | Median Mitems/s | IQR | Ratio to mpmc |
| --- | ---: | ---: | ---: |
| mpmc-16B | 3.5 | 0.0 | baseline |
| rigtorp | 2.6 | 0.1 | 0.73x |
| boost | 1.1 | 0.0 | 0.32x |

## Ping-pong latency

Closed-loop round-trip nanoseconds between two 64-entry
queues. Workers busy-spin. Values are medians of per-round
quantiles; brackets show the interquartile range across
rounds. Maximum is diagnostic, not an upper bound.

| Queue | Mean | p50 | p99 | p99.9 | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| mpmc-16B | 161.5 | 167.0 [0.0] | 208.0 [0.0] | 292.0 | 54625.0 |
| rigtorp | 138.1 | 125.0 [0.0] | 167.0 [0.0] | 291.0 | 30500.0 |
| boost | 334.3 | 333.0 [0.0] | 375.0 [0.0] | 500.0 | 40250.0 |

The measured steady-clock tick is about 42.0 ns. Differences
smaller than two ticks are below the timer's practical
resolution.

## What the differences mean

These are observations from this workload, not universal
rankings. The measured medians for the symmetric primary
topologies are:

- 1P/1C: mpmc-16B 198.9 M items/s; rigtorp 202.8 M items/s (1.02x, unresolved); boost 8.2 M items/s (0.04x, slower).
- 2P/2C: mpmc-16B 21.1 M items/s; rigtorp 20.5 M items/s (0.98x, unresolved); boost 5.8 M items/s (0.28x, slower).
- 4P/4C: mpmc-16B 4.9 M items/s; rigtorp 5.0 M items/s (1.05x, unresolved); boost 2.5 M items/s (0.51x, slower).

The relevant implementation facts are:

- mpmc-16B publishes payload and sequence with one 16-byte
  compare-exchange, then advances the shared index with a
  monotonic relaxed compare-exchange. rigtorp uses a split
  head/tail index with an 8-byte compare-exchange and a
  separate slot publication store; its pinned revision uses
  64-byte slot spacing on this 128-byte-cache-line host. Those
  differences change contention and footprint, but this
  benchmark does not isolate the contribution of CAS width,
  stride, index update, or memory order.
- At higher contention the differences narrow and become
  sensitive to cache-line ownership and scheduling. The 8P/8C
  row is explicitly a scheduler-stress measurement.
- Boost.Lockfree uses linked nodes, tagged handles, and a
  reusable freelist; it performs more metadata operations per
  transfer than a direct slot ring.

## Exact validation

Before the timed rounds, every library and topology transferred
1,048,576 unique values through an exact per-consumer bitmap.
All 18 exact cases reported `ok`. This pass is
outside the timed region and is not part of the rate above.

## Excluded library

- `erez-16B`: pinned upstream revision aef66f268f2b32143eb93ef857529025349c1f48 reads a sequence field non-atomically while other threads atomically replace the whole cell; excluded from the validated ranking.

The upstream erez-strauss source is not part of the validated
ranking. A corrected fork would need a separate name, patch
hash, and fresh reference run.

## Reproduce

```sh
./benchmarks/comparison/run.sh
```

The runner configures a Release build, fetches the pinned
dependency, runs both the general and specialized 1P/1C
profiles, then publishes `reference.json`, this file, and `SPSC.md` with
same-filesystem renames. A failed or unstable run leaves the previous reference
untouched. `--quick` is diagnostic only and never publishes.
