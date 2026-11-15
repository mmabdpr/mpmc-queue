# SPSC throughput profile

This profile is a topology-specific diagnostic, not part of the
stability-gated general ranking. It runs one producer and one consumer with
strict FIFO validation.

- Host: Apple M3 Max, 14 hardware threads
- OS: Darwin 27.2.0 (arm64)
- Compiler: Apple LLVM 21.0.0 (clang-2100.4.1.1)
- Capacity: 4096
- Items per trial: 150000000
- Rounds: 18

| Queue | Median Mitems/s | IQR | Min | Max |
| --- | ---: | ---: | ---: | ---: |
| mpmc-spsc | 276.8 | 35.9 | 231.4 | 300.7 |
| boost-spsc | 28.6 | 1.0 | 27.4 | 36.9 |

The `mpmc-spsc` IQR is 13% of its median. The profile reports that variability
and does not apply the general suite's stability gates. Both implementations
start from an empty queue after warm-up and validate every value against the
expected FIFO sequence. `mpmc-spsc` publishes a separate payload/publication
sequence pair with a release store. Use the general MPMC comparison for mixed
roles.
