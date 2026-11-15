# mpmc_queue

[![CI](https://img.shields.io/github/actions/workflow/status/mmabdpr/mpmc-queue/ci.yml?branch=main&label=CI)](https://github.com/mmabdpr/mpmc-queue/actions/workflows/ci.yml?query=branch%3Amain)
[![Release](https://img.shields.io/github/v/release/mmabdpr/mpmc-queue?label=release&sort=semver)](https://github.com/mmabdpr/mpmc-queue/releases/latest)
[![License: CPAL-1.0](https://img.shields.io/badge/license-CPAL--1.0-blue)](LICENSE)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
![Platform: Linux and macOS](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey)
![Header-only](https://img.shields.io/badge/library-header--only-blue)
[![TLA+ bounded model check](https://img.shields.io/badge/TLA%2B-bounded%20model%20check-informational)](verification/README.md)

A header-only C++20 implementation of a bounded, lock-free
multi-producer/multi-consumer queue. The design follows Erez Strauss's CppCon
2023 talk *Multi Producer, Multi Consumer, Lock Free Atomic Queue* and adds
cardinality policies, a dedicated SPSC queue, a POSIX shared-memory adapter, a
test suite, and an opt-in cross-library benchmark.

The queue targets low-latency code. Storage is allocated during construction.
Cell-based and SPSC `try_push`/`try_pop` operations do not allocate, lock, or
make system calls. Ownership wrappers can destroy an existing output object;
user callbacks and destructors are outside this guarantee. A queue cell holds
both the payload and its sequence marker, so publishing a value and making it
visible is one atomic compare-exchange.

```cpp
#include <mpmc/mpmc.hpp>

#include <cstdint>

int main() {
    // 4-byte payload + 4-byte index: one portable 8-byte cell.
    mpmc::mpmc_queue<std::uint32_t, 1024, std::uint32_t> queue;

    std::uint32_t sequence = 0;
    std::uint32_t value = 0;
    if (queue.try_push(42, sequence) && queue.try_pop(value, sequence)) {
        // value == 42; sequence is the queue position.
    }
}
```

## Features

- Bounded MPMC queue with queue-wide lock-free progress. Individual operations
  are not wait-free and no thread has a fairness guarantee.
- One atomic compare-exchange publishes the payload and readiness marker
  together, eliminating the reserve-then-write window of index-first queues.
- Compile-time or run-time capacity, with a power-of-two size.
- Exception-free setup through `std::error_code`; queue operations are
  `noexcept`.
- 8-byte cells when `sizeof(T) + sizeof(Index) <= 8`, otherwise 16-byte cells.
- Cooperative index advancement, sequence-returning push/pop, peek,
  conditional pop, consume, replace/conflation, and drop-oldest push.
- Cardinality policies for MPMC, MPSC, and SPMC use, plus a separate
  `spsc_queue` fast path with separate payload and publication-sequence fields.
- `unique_ptr_queue` for owning non-trivial objects in one address space.
- POSIX shared-memory adapter with a versioned header and bootstrap locking.
- Optional C++23 `std::expected` factories when the standard library provides
  `std::expected`; C++20 remains the minimum.

## Requirements

- Tested on Ubuntu 26.04 (GCC 15.2 and Clang 22.1) and macOS 26 (Apple Clang
  21.0) with C++20 and CMake 4.5.0. The library requires CMake 3.21 or newer.
  Other POSIX systems are untested. Windows and MSVC are not supported.
- A target where `std::atomic<Cell>` is always lock-free for the selected 8- or
  16-byte cell. The library enforces this with a static assertion.
- 16-byte cells depend on the target and toolchain. On x86-64 GCC and Clang,
  `-mcx16` is required, but some GCC releases still report 16-byte atomics as
  not always lock-free. Linking `libatomic` does not satisfy the requirement.
  Such targets must use 8-byte cells.
- AArch64 support for 16-byte atomics depends on the compiler's LL/SC or LSE
  lowering. Use the static assertion as the source of truth.
- `spsc_queue` requires a lock-free `std::atomic<Index>` and can run on
  8-byte-cell targets.

CMake probes the actual cell types with the same options consumers receive.
The probe selects tests, examples, and benchmark cases; it does not relax the
static assertion for a directly instantiated queue. On an 8-byte-only target,
16-byte-only tests, `unique_ptr_queue`, and 16-byte benchmark cases are omitted.
`-DMPMC_FORCE_8_BYTE_CELLS=ON` and the `portable8` preset exercise that path on
a 16-byte-capable machine.

## Build and install

Tagged releases are published on the [Releases](https://github.com/mmabdpr/mpmc-queue/releases/latest)
page with a complete source archive and a headers-and-documentation archive.
The source archive is the supported route for CMake and pkg-config installation.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

For an install-only build without tests or examples:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DMPMC_BUILD_TESTS=OFF -DMPMC_BUILD_EXAMPLES=OFF
cmake --install build --prefix "$HOME/.local"
```

When installing to a non-default prefix, add it to `CMAKE_PREFIX_PATH` before
configuring a consumer. `/usr/local` may require elevated permissions.

| Option | Default | Meaning |
| --- | --- | --- |
| `MPMC_BUILD_TESTS` | ON at top level | Build the test suite |
| `MPMC_BUILD_EXAMPLES` | ON at top level | Build examples |
| `MPMC_BUILD_BENCHMARKS` | OFF | Build the throughput/latency benchmark |
| `MPMC_BUILD_COMPARISON_BENCHMARKS` | OFF | Build the cross-library comparison |
| `MPMC_ENABLE_MCX16` | ON | Forward `-mcx16` on x86-64 when the probe confirms 16-byte support |
| `MPMC_FORCE_8_BYTE_CELLS` | OFF | Build test/example/benchmark targets with 8-byte cells |

The library is an INTERFACE target, `mpmc::mpmc_queue`.

CMake:

```cmake
find_package(mpmc_queue CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE mpmc::mpmc_queue)
```

pkg-config:

```sh
c++ -std=c++20 $(pkg-config --cflags mpmc_queue) app.cpp \
    $(pkg-config --libs mpmc_queue)
```

`pkg-config --cflags` provides the include path and, when configured, `-mcx16`.
It does not set `-std=`. Use C++20 or newer. `--libs` adds `-pthread` where
needed and `-lrt` on glibc versions that require it for `shm_open`.

The install includes the README, changelog, license and notice files, design
document, verification model and script, and benchmark reports under
`<prefix>/share/doc/mpmc_queue/`.

## API

### Queue types

```cpp
template<class T, std::size_t Capacity = 0, class Index = std::uint64_t,
         bool LazyPush = false, bool LazyPop = false,
         concurrency Mode = concurrency::mpmc>
    requires queue_payload_for<T, Index> &&
             (Capacity == 0 || valid_static_capacity<Capacity, Index>)
class mpmc_queue;
```

| Type | Contract |
| --- | --- |
| `mpmc_queue<...>` / `bounded_queue<...>` | Any number of producers and consumers |
| `spmc_queue<...>` | One producer; several consumers |
| `mpsc_queue<...>` | Several producers; one consumer and one observer role |
| `spsc_queue<...>` | Exactly one producer and one consumer |

The single-role contracts are correctness requirements, not tuning hints. The
queue types use C++20 concepts for payload, index, and capacity constraints:

| Concept | Requirement |
| --- | --- |
| `supported_index<Index>` | Unqualified unsigned integer of 1, 2, 4, or 8 bytes |
| `queue_payload<T>` | Complete, trivially copyable, non-cv object |
| `queue_payload_for<T, Index>` | Payload and index fit in an 8- or 16-byte cell |
| `valid_static_capacity<Capacity, Index>` | Zero or a power of two within the index horizon |
| `unique_ptr_payload<T>` | Non-array object for `unique_ptr_queue` |
| `shared_memory_payload<T>` | Queue payload, standard-layout, and approved for shared memory |

For cell-based queues, `T` must be trivially copyable and
`sizeof(T) + sizeof(Index)` must not exceed 16 bytes. A 64-bit index leaves 8
payload bytes; a 32-bit index leaves 12. Larger values must be passed by
handle: `unique_ptr_queue` in-process or an index/handle in shared memory.
The dedicated `spsc_queue` stores the payload directly in its slots and does not
have the 16-byte cell limit; it still requires a trivially copyable,
nothrow-default-constructible `T`.

The payload is copied as raw bytes, including padding. `try_replace` with an
expected value can be sensitive to indeterminate padding; prefer packed
payloads or `try_replace(sequence, desired)`.

### Operations

| Operation | Result |
| --- | --- |
| `try_push(value)` / `push(value)` | `true` on success, `false` when full or sequence-exhausted |
| `try_push(value, sequence)` | Also returns the queue position |
| `try_pop(value)` / `pop(value)` | `true` on success, `false` when empty |
| `try_pop(value, sequence)` | Also returns the consumed position |
| `try_pop()` | `std::optional<T>` |
| `try_peek(value)` / `peek(value)` | Observe the head without consuming |
| `try_pop_if(pred, value[, sequence])` | Consume the head if the predicate accepts it |
| `consume(callback)` | Drain until empty or the callback returns `true` |
| `empty()` | Whether no entry is currently consumable |
| `size()` | Approximate item count |
| `capacity()` | Queue capacity |
| `publication_limit()` | Exclusive upper bound on publishable positions |
| `sequence_exhausted()` | Whether an index reached the publication limit |
| `publications_remaining()` | Snapshot of remaining publication positions |
| `clear()` | Reset to empty and start a new sequence generation |
| `is_lock_free()` | Static lock-free check for the selected cell |
| `entry_size` / `payload_size` | Cell size and payload capacity in bytes |
| `storage_bytes` | Inline slot storage for a compile-time capacity |
| `create(...)` | Heap-allocate a fixed- or run-time-capacity queue |

Run-time queues are created without throwing:

```cpp
std::error_code error;
auto queue = mpmc::mpmc_queue<T>::create(capacity, error);
if (!queue) {
    // error is invalid_capacity or allocation_failed
}
```

`mpmc::make_queue<Queue>(error, args...)` constructs a fixed-capacity queue on
the heap. Use it when inline storage is too large for the stack:

```cpp
auto queue = mpmc::make_queue<mpmc::mpmc_queue<T, 65536>>(error);
```

### Sequence horizon

The ready bit shares the marker word with the position. New publications stop
at `2^(bits-1) - capacity`; a marker never wraps onto an old position. With the
default 64-bit index this is about 9.2e18 publications. A 32-bit index gives
about 2.1e9.

Published entries remain drainable at the limit. `sequence_exhausted()`
reports the condition, and `clear()` starts a new generation. `clear()` requires
all operations, including observers, in every thread and attached process, to
be quiescent. It invalidates every previously returned sequence token; a token
from the old generation can equal a position in the new generation.

`sequence_exhausted()` and `publications_remaining()` are diagnostic snapshots.
In lazy modes an index can lag the slot state until a later operation helps, so
these values may be briefly stale.

### Errors, predicates, and callbacks

The library does not throw. Fallible setup takes `std::error_code&` and returns
null or `false`. Errors are in `mpmc::errc`:

`invalid_capacity`, `invalid_name`, `name_too_long`, `allocation_failed`,
`already_exists`, `not_found`, `not_ready`, `mapping_too_small`,
`incompatible_layout`.

OS failures use `std::system_category()`. Queue operations use `false` for
full, empty, and sequence exhaustion.

`try_pop_if` accepts a predicate returning exactly `bool`. `consume` accepts a
callback returning exactly `void` or `bool`. Both must be `noexcept` and are
invoked as `predicate(const T&, Index)` with the sequence as an `Index` prvalue.
By-value, `const Index&`, and `Index&&` parameters are accepted; an
`Index&`-only parameter is rejected. `try_pop_if` may invoke the predicate more
than once for the same entry if a concurrent replacement changes it, so the
predicate must be side-effect free.

`try_pop_if` and `consume` require a nothrow-default-constructible `T`. The
value-returning `try_pop()` also requires a nothrow destructor and a nothrow
move or copy path. Reference pop and peek do not require default construction.

### Cardinality and SPSC queues

`mpmc_queue` accepts a trailing `concurrency` argument. `spmc_queue` and
`mpsc_queue` are aliases with a single-producer or single-consumer contract.
The default `bounded_queue` remains general MPMC.

`spsc_queue<T, Capacity, Index>` is a restricted fast path. It has no replace,
conditional-pop, consume, or lazy-mode API. Each slot stores the payload and a
separate atomic sequence; the producer release-stores the publication marker,
and the consumer acquire-loads it, copies the payload, and release-stores the
next empty marker. The hot path has no whole-cell compare-exchange.

`spsc_queue` requires a trivially copyable, nothrow-default-constructible
payload and a lock-free `std::atomic<Index>`. `empty()` and `size()` are
approximate observations. Enqueue and dequeue must come from the designated
producer and consumer threads.

### Replacement and lazy modes

```cpp
bool try_replace(sequence, expected_value, desired_value); // exact CAS
bool try_replace(sequence, desired_value);                 // replace if live
bool try_replace_last(desired_value[, &sequence]);         // newest entry
bool try_push_or_replace(value[, &sequence]);              // replace or push
bool push_keep_n(value);                                   // drop oldest when full
```

These operations are best-effort under concurrency. `try_replace(sequence,
desired)` and `try_replace_last` preserve the existing sequence marker.
`push_keep_n` is non-blocking drop-oldest; under concurrency another consumer
may take the entry that made room.

`LazyPush=true` and `LazyPop=true` leave index advancement to the next
participant. This can reduce index-line traffic in some workloads, at the cost
of a coarser `size()` and more helping work later. Correctness is unchanged.
All processes attached to a shared-memory queue must use the same lazy flags;
attach validates them.

### Owning values

```cpp
mpmc::unique_ptr_queue<MyObject, 256> queue;

if (!queue.try_push(std::make_unique<MyObject>(...))) {
    // Full, exhausted, or rejected null: the temporary unique_ptr is destroyed.
}
std::unique_ptr<MyObject> item;
if (!queue.try_pop(item)) {
    // empty
}
```

A failed push leaves ownership with the caller. The wrapper transfers a raw
pointer through one process. Destruction or `clear()` deletes every remaining
object. Runtime capacity uses `unique_ptr_queue<T>::create(capacity, error)`.

### Shared memory

```cpp
using queue_type =
    mpmc::shared_memory_queue<std::uint32_t, std::uint32_t>;

std::error_code error;
auto producer = queue_type::create("/my-queue", 4096, error);
if (!producer) {
    return 1;
}
if (!producer->try_push(123)) {
    return 1;
}

auto consumer = queue_type::attach("/my-queue", error);
if (!consumer) {
    return 1;
}
std::uint32_t value = 0;
if (!consumer->try_pop(value)) {
    return 1;
}

if (!queue_type::remove("/my-queue", error)) {
    return 1;
}
```

`create()` returns `already_exists` if the name is in use. A creator keeps the
name after destruction by default; call `remove()` when no new process should
attach. Pass `shared_memory_lifetime::unlink_on_destroy` to `create()` for
RAII-managed deployments. Auto-unlink removes the name only when it still
refers to the creator's object; existing mappings remain valid, and future
attaches fail. It is best-effort during destruction.

Names may include or omit the leading `/`. A second slash, a null character, an
empty name, or an overlong name is rejected. macOS enforces the POSIX limit of
31 bytes including the slash.

Creation and attachment are serialized by an advisory lock file under `/tmp`,
keyed by user id and a hash of the queue name. The lock is held only during
create/attach, never on the push/pop path. A crashed creator leaves a clean,
uninitialized name that can be removed. The lock file remains in
place; deleting it can split participants across different lock inodes.

Shared-memory participants must be trusted, cooperating processes with the same
effective user and the same shared-memory and lock namespaces. They must agree
on architecture, data model, native endianness, atomic lowering, `AbiTag`, and
payload layout. Header validation detects truncated, mismatched, or
half-initialized mappings; it does not protect a live mapping from a malicious
same-user writer.

`shared_memory_queue<T, Index, LazyPush, LazyPop, AbiTag>` validates the
application tag, sizes, alignments, endianness, capacity, and lazy flags on
attach. Arithmetic and enum payloads are accepted by default. A class payload
must specialize `mpmc::is_shared_memory_safe<T>` as a promise that it is
trivially copyable, standard-layout, and free of process-local state:

```cpp
struct quote {
    std::uint32_t bid;
    std::uint32_t ask;
};

namespace mpmc {
template <>
struct is_shared_memory_safe<quote> : std::true_type {};
}
```

`instance_id()` is a nonzero diagnostic identifier for the mapped instance.
Moved-from handles are detached; check `valid()` before use. For durable or
versioned inter-process data, use a wire format rather than raw C++ bytes.

The shared-memory adapter is POSIX-only. On Linux, creation reserves backing
pages with `posix_fallocate` where available; OS memory pressure can still
terminate a process with `SIGBUS` or OOM outside the C++ error path.

## Guarantees and limitations

- **Linearization point:** for cell-based queues, a successful push or pop takes
  effect at its cell compare-exchange. The dedicated SPSC queue publishes at its
  sequence store. The informal argument is in `docs/design.md`; the TLA+ model
  checks safety of a finite abstraction.
- **Lock-free progress:** a stalled thread cannot stop other threads from
  completing operations. Operations are not wait-free, and no individual
  thread is guaranteed to win a compare-exchange.
- **No hot-path allocation or system calls** for cell-based and SPSC push/pop.
  Shared-memory create/attach performs mapping and system calls. Ownership
  wrappers can destroy an existing output object, and user callbacks and
  destructors are outside this guarantee.
- **Fixed capacity:** no growth, blocking, or backpressure policy. Callers
  handle `false` for full and empty.
- **Sequence horizon:** new publications stop at `2^(bits-1) - capacity`.
  Published entries remain drainable; `clear()` starts a new generation.
  See `clear()` for the quiescence requirement.
- **No per-thread fairness:** the queue as a whole makes progress; individual
  threads may lose compare-exchange races. Applications that need service
  fairness should shard work or add a dispatcher.
- **Fixed 128-byte slot stride for cell-based queues:** adjacent slots do not
  share a cache line on 64- or 128-byte-line targets. The stride is part of the
  shared-memory layout. `spsc_queue` stores its own slots and is not part of
  that ABI.
- **Shared-memory lifecycle is application-managed:** auto-unlink is opt-in and
  best-effort; crashed creators can leave stale names. All participants must be
  trusted and layout-compatible.
- **Bounded verification:** the TLA+ model checks finite capacities 1, 2, and 3.
  It is not a proof of the C++ memory model, compiler lowering, or hardware
  atomicity.

## Performance

- Use `spsc_queue` for a strict single-producer/single-consumer path. It avoids
  the whole-cell compare-exchange; measure it on the target workload, since
  its advantage depends on runtime, core placement, and contention.
- Use `spmc_queue` or `mpsc_queue` only when the single-thread role is
  guaranteed.
- The general MPMC path uses relaxed index hints and relaxed preliminary cell
  loads. The cell compare-exchange carries the release/acquire publication
  ordering. Hints are monotonic but can lag the cells.
- An 8-byte cell uses a smaller atomic; measure both cell sizes on the target.
- Keep producers and consumers on the same NUMA node as the queue storage.
  For shared memory, allocate the mapping on the node used by the busiest
  readers.
- Capacity trades latency for jitter tolerance. Larger capacities touch more
  cache lines. Compile-time capacities store slots inline at 128 bytes per
  slot; use the run-time form or `make_queue` for large queues.
- Avoid `size()` in a hot loop; it is a diagnostic, not a synchronization
  primitive.
- Pin threads and isolate cores when measuring tail latency.

### Benchmark

Build with `-DMPMC_BUILD_BENCHMARKS=ON`. The sweep covers 8-byte cells and,
where supported, 16-byte cells; eager and lazy modes; capacities 256 and 4096;
and 1/2/4 producer and consumer pairs. It uses a coordinated start, warm-up,
and repeated timed runs. Consumers count locally and aggregate after join. The
1P/1C ping-pong test reports round-trip latency percentiles.

| Option | Meaning |
| --- | --- |
| `--items N` | Items per timed run (default 500000) |
| `--reps N` | Timed repetitions, 1–1000 (default 3) |
| `--latency-samples N` | Ping-pong samples (default 50000) |
| `--quick` | Reduced-workload run |
| `--check` | Cell-selection self-check, then exit |
| `--help` | Print usage |

Numeric options use strict `std::from_chars` parsing. The benchmark rejects
workloads beyond the queue's publication horizon and aborts if a retry observes
`sequence_exhausted()`.

### Cross-library comparison

The opt-in suite in `benchmarks/comparison/` compares this implementation with
Boost.Lockfree `boost::lockfree::queue` and
[rigtorp/MPMCQueue](https://github.com/rigtorp/MPMCQueue). It uses calibrated
workloads, balanced rounds, timed count and checksum validation, a separate
exact per-item pass, and host/stability gates. See
`benchmarks/comparison/README.md` for the protocol and
`benchmarks/comparison/RESULTS.md` for measured results.

```sh
./benchmarks/comparison/run.sh           # reference run
./benchmarks/comparison/run.sh --quick   # diagnostic only
```

A successful run publishes `reference.json`, `RESULTS.md`, and the separate
1P/1C `SPSC.md` report. The first run fetches the pinned rigtorp header and
requires Boost. The current reference used `--allow-busy-host`; its host load
and the one asymmetric diagnostic are recorded in `RESULTS.md`.

The pinned erez-strauss implementation is excluded from the validated ranking;
see `benchmarks/comparison/README.md` for the exact revision and reason.

## Tests and verification

The C++ tests are self-contained and offline. They cover FIFO edges, lazy
modes, narrow indices, sequence exhaustion, ownership, MPMC stress with
duplicate/loss detection, concurrency policies, payload integrity, shared
memory bootstrap and ABI checks, cross-process producer/consumer tests, and a
`-fno-exceptions` build. Conditional targets cover `unique_ptr_queue` and C++23
`std::expected`.

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

The `clang-tidy` target applies the repository configuration to every
translation unit and treats diagnostics as errors. The `format-check` target
checks changed files. `portable8` exercises the 8-byte-cell path.

A bounded TLA+ model is in `verification/`:

```sh
./verification/run-tlc.sh
```

It checks finite capacities 1, 2, and 3 with two producers and two consumers.
The runner verifies a pinned TLA+ tools jar, uses a temporary scratch directory,
and retains counterexample artifacts on failure. See
`verification/README.md` for the checked properties and limits.

### CI toolchain

CI uses explicit runner images, compiler names, and hash-locked tool archives.
The exact versions are in [`ci/toolchain.lock`](ci/toolchain.lock): Ubuntu 26.04
with GCC 15.2 or Clang 22.1, macOS 26 with Xcode 26.6 and Apple Clang 21.0,
CMake 4.5.0, Python 3.14.10, JDK 21.0.13+9, and clang-format 23.1.5. GitHub
Actions are pinned to full commit SHAs. `ci/install-tools.sh` verifies every
download against a SHA-256 digest, and `ci/verify-toolchain.sh` rejects version
drift before the build starts.

The GitHub-hosted runner kernel, image contents, hardware, and availability of
the macOS/Xcode image remain external dependencies. The immutable release, its
tag, and `SHA256SUMS` are the durable record; see [`RELEASING.md`](RELEASING.md)
for refresh and recovery procedures.

## Acknowledgements

The queue design and API are based on Erez Strauss's CppCon 2023 talk *Multi
Producer, Multi Consumer, Lock Free Atomic Queue*. The reference implementation
he discussed is
[erez-strauss/lockfree_mpmc_queue](https://github.com/erez-strauss/lockfree_mpmc_queue)
(MIT). This project is an independent C++20 implementation of the published
queue design, with additional packaging, tests, diagnostics, and a shared-memory
adapter.

## License

CPAL-1.0. See `LICENSE` and `NOTICE`.

Commercial and proprietary Larger Works may use the library. If you distribute
or externally deploy modified covered files, those files must be made available
in source form under CPAL-1.0. Executable distributions must state how recipients
can obtain the covered source. Keep the file notices, the license text, and a
record of changes with the source.

Private internal modifications are not subject to the CPAL External
Deployment source-availability obligation. External Deployment includes use
by anyone other than you, including over a network. A Larger Work with a
graphical interface must display the Exhibit B attribution at launch; headless
products do not. CPAL-1.0 is generally considered incompatible with the GPL.

This is not legal advice. Companies should review the license with their own
counsel.
