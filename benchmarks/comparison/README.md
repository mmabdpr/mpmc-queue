# Cross-library MPMC queue comparison

This directory contains the opt-in benchmark that compares
`mpmc::mpmc_queue` with two other bounded queues:

- Boost.Lockfree `boost::lockfree::queue`
- [rigtorp/MPMCQueue](https://github.com/rigtorp/MPMCQueue)

The suite produces one canonical general-MPMC result (`reference.json` and
generated [RESULTS.md](RESULTS.md)) and one topology-specialized result
([SPSC.md](SPSC.md)). It is not part of the default build or test suite. A full
run needs Boost, network access on first use for the pinned rigtorp header, and
lock-free 16-byte atomic cells.

## Quick start

```sh
./benchmarks/comparison/run.sh
```

`run.sh` fetches the pinned dependency, configures a Release build, runs the
reference protocol, validates the result, publishes `reference.json` and
`RESULTS.md`, then runs the specialized 1P/1C profile and publishes `SPSC.md`.
All three outputs use same-filesystem renames. A failed or unstable run leaves
the previous reference untouched. A full reference run takes tens of minutes; the
calibrated workload and the 18-round protocol determine the exact duration.

For a short diagnostic run that is never published:

```sh
./benchmarks/comparison/run.sh --quick
```

The executable also accepts `--items N`, `--latency-samples N`, `--max-load X`,
and `--allow-busy-host`; `run.sh` forwards these options. `--quick` is intended
for build and format checks, not for performance decisions.

## Eligibility

Rigtorp/MPMCQueue is pinned at
`b9808ede08f26fa9df4df4e081d19cace8f6c6ea`. The pinned erez-strauss
`lockfree_mpmc_queue` revision
`aef66f268f2b32143eb93ef857529025349c1f48` is excluded from the validated
ranking. Its sequence field is read non-atomically while another thread can
replace the whole cell atomically, and its atomic access uses non-portable type
punning. A corrected fork would need a separate name, a patch revision, and its
own reference run. The exclusion is recorded in `reference.json` and in the
generated results.

## Methodology

- Payload: 64-bit unsigned integer. mpmc-16B uses
  `mpmc::mpmc_queue<uint64_t, 4096, uint32_t>`: an 8-byte payload plus a 32-bit
  index in a 16-byte atomic cell. Boost.Lockfree and rigtorp use their own
  internal layouts. The comparison is of queue APIs and behavior, not
  byte-for-byte identical storage.
- Throughput capacity: 4096 entries. A preflight checks that each queue accepts
  exactly 4096 items, preserves FIFO order, and drains to empty.
- Primary topologies: 1P/1C, 2P/2C, 4P/4C, 4P/1C, and 1P/4C. Each topology is
  measured in 18 balanced rounds. Rounds are round-major: each round visits the
  five topologies in a seed-shuffled order, and within each topology visits all
  three libraries in one of the six permutations. The permutations repeat three
  times, so each library appears six times in each position.
- Workload: each library and topology has its own item count, calibrated from
  warmed pilots at that topology to a 1.2 s target with a 250 ms acceptance
  floor. Counts are rounded to a multiple of 64 and capped at 134,217,728
  items. `--items` fixes every case to the same count and must be a multiple of
  64.
- Throughput timing: workers spin on release/acquire gates. Thread creation,
  warm-up, and teardown are outside the measured interval. Each round uses a
  fresh queue. Before timing, the same workers and the same queue run an
  untimed warm-up covering at least sixteen capacity laps; the queue is drained
  and the timed generation starts empty. The measured interval begins
  immediately before the timed release and ends at the latest worker finish,
  before joins.
- Validation: every producer pushes a unique `(producer, item)` value. Timed
  consumers keep only register-resident count, wrapped 64-bit sum, and wrapped
  sum-of-squares over every transferred item. These deterministic checks detect
  loss, duplication, or value corruption but are not exhaustive. Before the
  timed rounds, a separate 1,048,576-item pass checks every library and
  topology item-by-item with an exact per-consumer bitmap. Invalid-value and
  final-empty checks are supplementary. A validation failure stops the run.
- Retry behavior: failed `try_push`/`try_pop` attempts use a non-blocking
  architecture hint (`PAUSE` on x86, `YIELD` on AArch64). Failed-attempt counts
  are recorded as diagnostics.
- Latency: 1,000,000 round-trip samples per round, 18 rounds, and 100,000
  warm-up samples. Two 64-entry queues carry a ping-pong exchange between the
  main thread and a busy-spinning responder. Reported quantiles are
  nearest-rank (`ceil(p*n)-1`), per round; the table shows the median across
  rounds and the interquartile range of those round quantiles.
- Thread placement: macOS uses `QOS_CLASS_USER_INTERACTIVE`, a scheduler hint
  rather than hard CPU affinity. Linux pins each worker to a distinct allowed
  CPU and verifies the resulting mask. A placement failure aborts a reference
  run. The macOS runner holds a `caffeinate` assertion and applies
  `taskpolicy -a -t 1 -l 1` for the duration of the run.
- Host gates: the run requires a quiet host by default, using the one-minute
  load average; a fixed-work sentinel is measured before the rounds and after
  every round, with a 10% drift limit. Symmetric primary cases must pass
  stability limits before publication: IQR/median at most 10%, scaled
  MAD/median at most 7.5%, bootstrap 95% confidence-interval width at most 15%,
  first-half/second-half drift at most 10%, a timed trial of at least 250 ms,
  and start skew at most 1%. An unstable asymmetric primary case is marked
  diagnostic and excluded from ranking claims. The 8P/8C case is a separate
  scheduler-stress measurement because it runs sixteen workers on a host with
  fewer logical CPUs; its start-skew bound is 10% to allow the deliberate
  oversubscription. Throughput and latency workers also have a 300 s trial
  deadline.
- Reference run: the current result was produced with `--allow-busy-host`, a
  start load of 7.94 against the 7.00 default limit. `RESULTS.md` records the
  override and the one asymmetric diagnostic.
- Statistics: per case the renderer reports median, quartiles, IQR, scaled MAD,
  minimum, maximum, and a fixed-seed bootstrap confidence interval. Paired
  per-round ratios to mpmc use the same round. A difference is marked clear
  only when the ratio's confidence interval lies wholly outside the
  0.95-1.05 equivalence band; otherwise it is unresolved. The displayed ratio
  is the median of paired per-round ratios, not the ratio of the displayed
  medians, so rounding can make the two differ slightly.

## SPSC specialized profile

The general comparison measures the whole-cell MPMC algorithm. For the
single-producer/single-consumer case, `spsc_raw.cpp` is a separate topology
profile. It compares:

- `mpmc-spsc`: `mpmc::spsc_queue<uint64_t, 4096, uint32_t>`, with separate
  payload and publication-sequence fields;
- `boost-spsc`: `boost::lockfree::spsc_queue` with fixed capacity 4096.

Both rows are single-producer/single-consumer queues. The general whole-cell
MPMC queue is measured in the ranked comparison above rather than mixed into
this topology-specific table.

The profile uses one producer and one consumer, a 150,000,000-item default
workload that starts from an empty queue after a warm-up, strict sequential
FIFO validation, and no per-item bitmap. A single consumer can validate every
item against its expected value, so this remains an exact
loss/duplication/order check while avoiding bitmap traffic in the measured
loop. The profile is a topology-specific diagnostic, not part of the
stability-gated general ranking; its IQR is reported but it does not use the
general suite's stability gates. Do not combine its rows with the 18-round
general table.

`run.sh` runs the full profile automatically after a successful general run.
To run it directly, build it from the comparison build directory:

```sh
cmake --build benchmarks/comparison/build --target mpmc_spsc_comparison
./benchmarks/comparison/build/benchmarks/comparison/mpmc_spsc_comparison
```

It accepts `--items N`, `--rounds N`, and `--quick`. The output is JSON lines
with one record per timed trial; the default is 18 rounds, and each round runs
both implementations in rotating order. A delayed operation aborts after a
bounded deadline instead of hanging the profile. Summarize it with
`spsc_render.py`:

```sh
./benchmarks/comparison/build/benchmarks/comparison/mpmc_spsc_comparison > spsc.jsonl
python3 benchmarks/comparison/spsc_render.py spsc.jsonl SPSC.md
```

The profile does not publish the general suite's canonical reference file and
has no host or stability gate. Use it on an idle host for a low-P/C
comparison; keep its table separate from the general MPMC table.

## Output

The benchmark emits JSON lines. `render_results.py` rejects quick runs,
incomplete round sets, validation failures, and unstable symmetric primary
cases, then writes:

- `reference.json`: canonical metadata, excluded-library record, exact
  validation records, sentinel samples, aggregated throughput cases, and
  aggregated latency cases.
- `RESULTS.md`: the human-readable tables and the source-level interpretation,
  generated from `reference.json`.

No individual trial rows or CSV reruns are retained. The canonical JSON is the
only stored result, so there is one reference per host configuration and one
reproduction command. If validation or a stability gate fails, the runner prints
the path of a temporary diagnostic JSON outside the repository and publishes
nothing.

The build uses the installed Boost (at least 1.83) and the pinned rigtorp
header. `fetch_deps.sh` verifies the exact revision and refuses a dirty checkout
tree. The pinned revision is recorded in `fetch_deps.sh` and this README; the
excluded erez revision is also recorded in `reference.json`. A normal library
build never fetches dependencies.

## Reproduce

```sh
./benchmarks/comparison/run.sh
```

Run on an otherwise idle host. On macOS the scheduler and QoS hints, thermal
state, and background indexing can still affect the tail. Runs with unstable
symmetric primary cases are rejected; unstable asymmetric cases are reported
as diagnostics.

## Files

- `compare.cpp`: general MPMC benchmark and queue adapters.
- `spsc_raw.cpp`: specialized 1P/1C raw-throughput profile.
- `render_results.py`: validation, statistics, and result generation.
- `spsc_render.py`: summary for the specialized profile.
- `CMakeLists.txt`: target wiring.
- `fetch_deps.sh`: pinned external source checkout.
- `run.sh`: fetch, build, run, and publish.
- `reference.json`: canonical validated result.
- `RESULTS.md`: generated general-MPMC report.
- `SPSC.md`: generated 1P/1C specialized report.
