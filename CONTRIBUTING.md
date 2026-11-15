# Contributing

Issues and pull requests are welcome. The project is tested on Ubuntu 26.04
(GCC 15.2 and Clang 22.1) and macOS 26 (Apple Clang 21.0) with C++20 and CMake
4.5.0. The library requires CMake 3.21 or newer. Other POSIX systems are
untested; Windows and MSVC are out of scope.

By submitting a contribution, you confirm that you have the right to license it
and agree that it is provided under CPAL-1.0.

## Before submitting

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --build --preset debug --target clang-tidy
```

The `clang-tidy` target applies `.clang-tidy` to every translation unit and
fails on diagnostics. Install clang-tidy and clang-format; CMake searches the
normal `PATH` and the Homebrew LLVM locations. CI checks formatting with the
pinned clang-format from [`ci/toolchain.lock`](ci/toolchain.lock).

Run the sanitizer preset for changes that affect the queue protocol, shared
memory, or ownership:

```sh
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Use the TSan preset for changes that affect concurrent publication, index
helping, or shared metadata:

```sh
cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

Changes to the cell/index protocol, helping, memory ordering, or shared-memory
layout also require the bounded TLA+ model:

```sh
./verification/run-tlc.sh
```

The runner verifies its pinned TLA+ tools jar on every invocation. A failed run
keeps its scratch directory and prints the counterexample and trace paths. Set
`KEEP_TLC_ARTIFACTS=1` to retain a successful run as well. Update
`verification/MpmcQueue.tla`, its invariants, and `docs/design.md` together
when changing the protocol.

For cell-size-sensitive changes, run the portable 8-byte preset:

```sh
cmake --preset portable8
cmake --build --preset portable8
ctest --preset portable8
```

Formatting is checked with:

```sh
cmake --build --preset debug --target format-check
```

CI checks changed lines against the pull-request base or previous push tip and
requires a full-history checkout. CI uses the exact runner, compiler, CMake,
Python, Java, clang-format, and CodeQL versions recorded in
[`ci/toolchain.lock`](ci/toolchain.lock); do not replace them with floating
versions. Refresh that lock deliberately and run the full workflow after any
change.

## Guidelines

- Keep the hot path allocation-free and system-call-free for in-process queues.
- Preserve single-cell publication: payload and readiness must be published by
  one atomic compare-exchange on the general MPMC path.
- Do not add locks or blocking waits to the core algorithm. A retry loop must
  follow an observable state change made by another participant.
- Add tests for behavioral changes, including lazy modes and shared-memory
  validation when relevant.
- Keep public headers self-contained and warning-clean with
  `-Wall -Wextra -Wpedantic`.
- Do not set `CMAKE_CXX_STANDARD` at directory scope. Publish C++20 with
  `target_compile_features(cxx_std_20)` so `add_subdirectory` consumers keep
  their own language mode.
- Do not add `-std=` to pkg-config `Cflags`; consumers choose their standard.
- Update `docs/design.md` when the state machine, memory ordering, or
  shared-memory layout changes. Update `README.md` when a public constraint,
  diagnostic, or lifetime rule changes.
- Keep the installed documentation layout intact.

The cross-library comparison is opt-in and not part of CI. Run
`./benchmarks/comparison/run.sh` for performance-sensitive queue or benchmark
changes; it needs Boost, network access on first use for the pinned rigtorp
header, and an otherwise idle host. Do not hand-edit its generated artifacts or
publish a quick run.

The release procedure is documented in [RELEASING.md](RELEASING.md).
