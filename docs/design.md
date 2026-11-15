# Design

Queue algorithm, memory ordering, shared-memory layout, and correctness
argument. The README covers installation and the public API.

## 1. Cell layout

A push publishes the payload and its readiness marker with one atomic operation:

```
8-byte cell:   [ sequence (1, 2, 4, or 8 bytes) ][ payload; unused bytes zero ]
16-byte cell:  [ sequence (1, 2, 4, or 8 bytes) ][ payload; unused bytes zero ]
```

```cpp
template<class Index, std::size_t CellBytes>
struct alignas(CellBytes) cell {
    Index seq;
    std::array<std::byte, CellBytes - sizeof(Index)> data;
};
```

The cell has no padding. `CellBytes` is 8 when `sizeof(T) + sizeof(Index) <= 8`,
otherwise 16 when the sum is at most 16. Larger combinations are rejected at
the specialization boundary by `queue_payload_for`.

The dedicated `spsc_queue` is the exception. Each slot stores the payload and a
separate publication sequence; the producer and consumer exchange a release
store and an acquire load instead of a whole-cell compare-exchange.

Each cell occupies a 128-byte slot:

```cpp
template<class Cell>
struct alignas(128) slot {
    std::atomic<Cell> value;
    // padding to 128 bytes
};
```

The 128-byte stride keeps adjacent slots on separate cache lines on 64-byte and
128-byte-line targets. The two index atomics are also 128-byte aligned and 128
bytes apart. The stride is fixed and is part of the shared-memory layout.

`std::atomic<cell_type>::is_always_lock_free` is asserted. Compiler flags and
CPU feature names are not authoritative. On x86-64 GCC/Clang, `-mcx16` is
required for 16-byte cells, but some GCC releases still do not make the atomic
always lock-free. Such targets use 8-byte cells.

## 2. Markers and slot states

A position is a monotonically increasing queue index. Capacity is a power of
two, so the slot for position `p` is `p & (capacity - 1)`. Positions are reused
one lap later at `p + capacity`.

Each cell stores:

```
marker = (position << 1) | ready
```

`ready == 0` means empty for that lap; `ready == 1` means published. The low bit
is the readiness flag. Positions below
`publication_limit = 2^(bits-1) - capacity` are publishable. The reserved tail
lets the final pop write the next-lap empty marker without wrapping. At the
limit, new publications stop. Published entries remain drainable, and `clear()`
starts a new generation under application-wide quiescence.

For a producer at position `write`, the cell marker has these meanings:

| Marker | Meaning |
| --- | --- |
| `write << 1` | Empty at this position; publish it |
| `(write << 1) \| 1` | Another producer already published this position |
| `((write - capacity) << 1) \| 1` | Previous lap still full; queue is full |
| `(write + capacity) << 1` | Already consumed; write index is stale |

For a consumer at `read`, `(read << 1) | 1` is consumable, `read << 1` is
empty, and a marker from a later lap means the position was already consumed.

The implementation uses differences instead of wrapped equality:

- `behind = write - marker_position`; `behind == capacity` means full.
- `behind > half_range` means the marker is ahead of `write`, already consumed.
- otherwise the snapshot is transient and is retried.

`half_range` is `numeric_limits<Index>::max() / 2`. Capacity is limited to
`max / 4`, leaving room for the marker shift and lap arithmetic.

`sequence_exhausted()` and `publications_remaining()` are diagnostic snapshots.
In lazy modes an index can lag the slot state until a later operation helps.

`clear()` resets both indices and every slot marker. It invalidates every
sequence token returned before the call; a token from the old generation can
equal a new position and name an unrelated entry. For shared memory, the
mapping and `instance_id()` remain unchanged, so positions observed by other
processes are invalid too. The caller must stop all threads and processes that
can access the queue before calling `clear()`.

## 3. Push

```
loop:
    write = load(write_index, relaxed)
    if write >= publication_limit: return false
    slot = slots[write & mask]
    cell = load(slot, relaxed)
    pos = marker_position(cell)

    if pos == write:
        if cell is empty:
            try CAS(slot, cell -> {payload, full_marker(write)},
                    release / relaxed)
            if success:
                sequence = write
                if !LazyPush: publish_write(write)
                return true
            continue
        else:
            publish_write(write)
            continue

    behind = write - pos
    if behind == capacity:
        return false
    if behind > half_range:
        publish_write(write)
        continue
```

A successful push writes the payload in the compare-exchange that makes the cell
ready. A producer that finds a cell already published by another producer helps
advance the write index. This lets the queue progress when the winning producer
is descheduled immediately after its compare-exchange.

`publish_write` is a monotonic hint update. It uses a compare-exchange in MPMC
and MPSC mode and a relaxed store in the single-producer SPMC and SPSC mode. A
delayed participant cannot move the hint backwards.

## 4. Pop

```
loop:
    read = load(read_index, relaxed)
    if read >= publication_limit: return false
    slot = slots[read & mask]
    cell = load(slot, relaxed)
    pos = marker_position(cell)

    if pos == read:
        if cell is empty:
            return false
        next_empty = empty_cell(read + capacity)
        try CAS(slot, cell -> next_empty, acquire / relaxed)
        if success:
            copy payload from the local cell snapshot into value
            sequence = read
            if !LazyPop: publish_read(read)
            return true
        continue

    ahead = pos - read
    if ahead < half_range:
        publish_read(read)
        continue
```

The payload is copied from the local snapshot, not from the shared cell after
the CAS. Once the compare-exchange writes the next-lap empty marker, a producer
may reuse the slot and overwrite its bytes; the local snapshot is private.

`try_pop_if` and `consume` also copy into a private candidate. `consume`
publishes the empty marker before invoking the callback. `try_pop_if` invokes
the predicate before its compare-exchange and may invoke it more than once if a
concurrent replacement changes the cell. Predicates must be side-effect free.

Predicates return exactly `bool`; consume callbacks return exactly `void` or
`bool`. Both must be `noexcept` and receive `(const T&, Index)` with the
sequence as an `Index` prvalue. By-value, `const Index&`, and `Index&&`
parameters work; an `Index&`-only parameter is rejected.

## 5. Memory ordering

| Operation | Ordering |
| --- | --- |
| Ordinary push/pop preliminary cell load | `relaxed` |
| Conditional pop and peek cell load | `acquire` |
| Push cell compare-exchange | success `release`, failure `relaxed` |
| Pop cell compare-exchange | success `acquire`, failure `relaxed` |
| Ordinary push/pop index loads | `relaxed` |
| Observer `empty`/`size` index loads | `acquire` |
| Index compare-exchange/store | `relaxed` hints |
| `try_replace` compare-exchange | `acq_rel` / `acquire` |

A producer's payload write precedes its release compare-exchange. A consumer's
acquire compare-exchange sees that payload. The consumer may publish the
next-lap empty marker before it finishes copying, but it reads the local
snapshot, so reuse cannot overwrite bytes being copied.

Index loads are only hints. The cell marker establishes empty, full, and
consumed state. A participant revalidates the cell for the position it uses and
helps observed full or consumed positions. This is why a lagging index does not
affect correctness and why `size()` is approximate.

## 6. Progress

Both indices are cooperative. A thread that sees a safe-to-advance slot
advances the index. A producer stalled after publishing cannot block other
producers; a consumer stalled after consuming cannot block other consumers.
Producers do not read the read index on the push path, and consumers do not
read the write index on the pop path, except for diagnostics and
`try_replace_last`.

The queue is lock-free as a whole, not wait-free. A compare-exchange can lose,
and no thread has a fairness guarantee. Every retry follows a state change made
by another participant. A retry loop must not wait on a condition that no other
participant can change.

Ticket gates or blocking locks would remove lock-free progress. Backoff,
`yield`, or randomization can reduce contention but do not provide starvation
bounds and can worsen tail latency. Service fairness requires separate queues
and an application-level dispatcher.

## 7. Lazy modes and cardinality policies

By default, a successful push advances the write index and a successful pop
advances the read index. In lazy mode the winner leaves the index for the next
operation, which observes the ready or empty cell and helps. This can reduce
index-line contention at the cost of a coarser `size()` and more work later.

The `concurrency` policy selects the permitted roles:

- `mpmc`: any producers and consumers.
- `spmc`: one producer; several consumers.
- `mpsc`: several producers; one consumer and observer role.
- `spsc`: one producer and one consumer.

The general MPMC path loads atomic index hints. Single-role paths can keep an
owner-local cursor and a shared mirror. The dedicated `spsc_queue` uses the
separate payload/sequence design described in section 1.

Lazy modes do not change the linearization point or cell protocol. All
processes attached to a shared-memory queue must use the same flags; attach
validates them.

## 8. Replacement and conflation

`try_replace(sequence, expected, desired)` constructs a full cell for the
position and compares it against the exact expected cell. It succeeds only if
both marker and payload still match.

`try_replace(sequence, desired)` loads a full cell at the position, writes the
new payload while preserving the marker, and compare-exchanges. This is the
atomic conflation primitive for "replace the latest value" workloads.
`try_replace_last` tries `write` first, then `write - 1`, covering the
lazy-push case. `try_push_or_replace` tries replacement first and falls back to
a normal push.

These operations are best-effort: a consumer may consume the entry or another
producer may replace it first. `false` is not a permanent condition.

`push_keep_n` is non-blocking drop-oldest. If the push finds the queue full, it
pops one entry and retries. Under concurrency "keep the newest" is approximate,
and another consumer may take the entry that made room. At the publication
horizon it returns `false` without draining the backlog.

## 9. Capacity and cache behavior

- Capacity is a power of two, from 1 to `numeric_limits<Index>::max() / 4`.
- Slot stride is fixed at 128 bytes and is not configurable.
- Adjacent slots do not share a cache line on 64- or 128-byte-line targets.
- The read and write indices are 128 bytes apart.
- Compile-time capacities store slots inline at 128 bytes per slot. Large
  capacities should use run-time capacity or `make_queue`.

## 10. Shared-memory layout

The POSIX adapter uses one named mapping:

| Offset | Size | Contents |
| --- | --- | --- |
| 0 | 128 | `shared_memory_header` |
| 128 | 128 | write index |
| 256 | 128 | read index |
| 384 | `capacity * 128` | slots |

The 128-byte header:

| Offset | Field |
| --- | --- |
| 0 | magic `0x4d504d4351554555` |
| 8 | version 2 |
| 12 | `cell_bytes` |
| 16 | `value_size` |
| 20 | `value_align` |
| 24 | `index_size` |
| 28 | `index_align` |
| 32 | `flags` (lazy modes) |
| 36 | `endianness` |
| 40 | `capacity` |
| 48 | `slots_offset` (384) |
| 56 | `mapping_size` |
| 64 | plain `uint32_t ready` |
| 68 | `reserved1` |
| 72 | `abi_tag` |
| 80 | `instance_id` |
| 88 | reserved to 128 |

No pointers or process-local handles are stored in the mapping. The payload
must be trivially copyable, standard-layout, and free of pointers and owning
handles. Arithmetic and enum payloads are accepted by default; a class payload
requires an explicit `is_shared_memory_safe<T>` specialization. `AbiTag` is the
application's schema contract, and endianness is validated independently.

Creation:

1. Lock the bootstrap lock file.
2. `shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600)`.
3. `ftruncate` to the logical size; on Linux, `posix_fallocate` reserves backing
   pages where available.
4. `mmap(MAP_SHARED)`.
5. Zero the header and placement-new the header, index atomics, and slots.
6. Initialize slot markers and indices.
7. Set `ready = 1` and release the lock.

Attach:

1. Take a shared lock on the bootstrap file.
2. `shm_open` and `fstat`.
3. `mmap`.
4. Read the plain `ready` word and validate the header.
5. Build the engine state over the mapped atomics and slots.
6. Release the lock.

The lock file lives under `/tmp`, keyed by user id and queue name. It is held
only during create/attach and left in place. All participants must share the
same `/tmp`, shared-memory namespace, effective user, architecture, endianness,
and atomic lowering. Header validation detects mismatched or half-initialized
mappings; it does not protect a writable mapping from a malicious same-user
process.

On Linux, `posix_fallocate` reserves backing pages where available. Memory
pressure can still cause `SIGBUS` or OOM outside the C++ error path.

Default lifetime keeps the name after the creator is destroyed. The
`unlink_on_destroy` policy removes the name only while it still identifies the
creator's object, using backing identity where available and the instance id as
a fallback. Existing mappings remain valid. Auto-unlink is best-effort; a
failed reopen, identity check, or unlink leaves the name for explicit
`remove()`. A forked child does not inherit the right to unlink the parent's
name.

## 11. Correctness

For a fixed position `p`, exactly one producer can change the slot from
`empty(p)` to `full(p)`: the compare-exchange expects the exact empty cell.
After publication, exactly one consumer can change it from `full(p)` to
`empty(p + capacity)`. A second consumer's compare-exchange fails and retries.
The payload is part of the cell, so a consumer's snapshot belongs to the
publication that won.

The write index advances only when the slot at the write position is full or
from a later lap. The read index advances only when the slot at the read
position has been consumed or is from a later lap. Advancing therefore never
skips a live entry. Each successful push returns a unique position, increasing
in linearized order.

Capacity at most `max / 4` and half-range difference checks prevent a marker
from another lap being mistaken for the live position while outstanding entries
are bounded by capacity. The publication limit reserves space for the final
pop's next-lap empty marker. At the limit new publications stop; published
entries remain drainable, and the queue does not wrap.

## 12. Non-goals

- Blocking push/pop or an internal backpressure policy.
- Dynamic growth.
- Per-thread fairness.
- Transparent support for non-trivially-copyable values. Use
  `unique_ptr_queue` in-process or a handle in shared memory.
- Windows/MSVC. The POSIX shared-memory adapter is the only backend.
- Automatic reclamation of stale shared-memory names after a crash.
- A machine-checked proof of the C++ memory model, compiler lowering, or
  hardware atomicity. The TLA+ model checks a finite safety abstraction; the
  tests and sanitizer builds cover the implementation.
