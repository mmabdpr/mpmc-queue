# Verification

`MpmcQueue.tla` is a bounded safety model of the cell and index protocol. It is
an abstraction of the algorithm, not a model of the C++ implementation.

## Model

- Push and pop are split into staged actions: load the index, load the cell, and
  compare-exchange using the loaded cell as the expected value. A thread can be
  suspended between any two stages.
- Local cell snapshots are represented by a validity bit and the index at which
  they were loaded. A slot write invalidates snapshots of that slot, which
  models the compare half of compare-exchange.
- Index advancement is a separate action, so a participant can pause after a
  successful cell compare-exchange and before moving the index. Other
  participants help, as in the queue. Eager and lazy modes differ only in when
  the advance is attempted; the model covers the window in both.
- Consumption is recorded per position and per consumer. The cell marker
  transition is the only guard against a second pop;
  `NoDuplicateConsumption` is checked as an invariant.
- Capacities 1, 2, and 3 are checked with two producers and two consumers.
  Capacity 3 exercises a non-power-of-two wrap pattern in the bounded model; the
  C++ queue accepts only power-of-two capacities.

## Checked invariants

- `SlotIndex` and `SlotBounds`: marker positions match slot indices and stay
  inside the bounded domain.
- `PublishedCell`: every full cell has a matching publication. The assertion
  does not check the contents of empty cells.
- `ConsumedWasPublished`: every pop removes the value published at that position.
- `NoDuplicateConsumption`: no position is popped twice by the same or different
  consumers.
- `PublicationUnique`, `PublishedPrefix`, `ConsumedPrefix`: publication values
  are unique and publication and consumption are prefix operations.
- `IndexWindow` and `OccupancyBound`: indices and occupancy stay within bounds.
- `LocalSnapshotsBounded`: local index snapshots do not exceed the
  unloaded-index sentinel. This invariant alone does not establish snapshot
  validity or load-stage ordering.

## Limits

The model does not prove:

- C++ acquire/release correctness or compiler lowering;
- that the selected `std::atomic<cell>` is lock-free or does not tear;
- behavior of `try_replace`, `try_pop_if`, `consume`, or `clear` beyond the
  simplified push/pop model;
- liveness or per-thread fairness;
- shared-memory bootstrap, ABI, or lifecycle behavior;
- the dedicated `spsc_queue` split sequence/payload implementation;
- C++ relaxed-memory observations and owner-local cardinality cursors. Index
  advancement is modeled as a separate action, so the model covers hints that
  lag published or consumed cells. Cells remain authoritative.

The test suite, sanitizer builds, and the correctness argument in
`docs/design.md` cover the implementation.

## Running the model

```sh
./verification/run-tlc.sh
```

The script downloads a pinned TLA+ tools jar into `${TMPDIR:-/tmp}` on first
use, verifies its SHA-256 on every invocation, and runs TLC in a temporary
scratch directory outside the repository. It stops at the first failing
configuration and keeps the scratch directory with the counterexample and trace
artifacts. Set `KEEP_TLC_ARTIFACTS=1` to keep a successful run.

## Direct C++ model checking

No model checker for the C++ source is integrated. GenMC 0.20.0 is the closest
fit for direct C++ weak-memory checking, but its 64-bit shared-value model
cannot represent 16-byte cells, and that release does not ingest this C++20
atomic path reliably. CBMC can check selected arithmetic and bounds properties
but does not explore weak-memory behavior.
