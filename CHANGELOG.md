# Changelog

## 1.0.0 - 2026-11-15

Initial public release.

### Queue
- Header-only bounded lock-free MPMC queue with 8- and 16-byte atomic cells,
  single-cell publication, and monotonic cooperative index hints.
- Compile-time and run-time capacity; sequence-returning push and pop, peek,
  conditional pop, consume, replacement/conflation, drop-oldest push, and lazy
  push/pop modes.
- `mpmc_queue`, `spmc_queue`, `mpsc_queue`, and the dedicated `spsc_queue` fast
  path for one producer and one consumer.
- `unique_ptr_queue` for owning non-trivial objects in one address space.
- Publication-horizon fail-stop, `clear()` generation reset, and heap factories
  for large compile-time queues.

### Interfaces
- C++20 concepts for payload, index, capacity, and ownership constraints;
  platform and layout guarantees remain `static_assert` checks.
- Exception-free setup through `std::error_code`; optional C++23
  `std::expected` factories where the standard library provides them.
- POSIX shared-memory queue with a versioned header, bootstrap lock,
  endianness/ABI-tag/instance-id validation, payload opt-in, and auto-unlink.
- CMake and pkg-config packages for the header-only target, including Threads
  and `librt` propagation.
- CPAL-1.0 licensing with SPDX headers, `NOTICE`, and installed license files.

### Verification and tooling
- Offline test suite covering FIFO edges, lazy modes, narrow indices, sequence
  exhaustion, ownership, MPMC stress, concurrency policies, shared-memory
  validation, and `-fno-exceptions` builds.
- Clang-tidy configuration with warnings treated as errors, a changed-file
  format check, ASan/UBSan and TSan presets, and an 8-byte-cell preset for
  targets without lock-free 16-byte atomics.
- Bounded TLA+ safety model and reproducible TLC runner.
- Exact CI toolchain pins with SHA-256-verified CMake, JDK, and clang-format
  downloads; GitHub Actions pinned by commit.
- Throughput/latency benchmark and opt-in cross-library comparison with
  calibrated workloads, timed count and checksum validation, a separate exact
  pass, host and stability gates, and generated reports.
