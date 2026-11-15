# Security

Report suspected vulnerabilities privately through
[GitHub Security Advisories](https://github.com/mmabdpr/mpmc-queue/security/advisories/new)
or by email to [git@mohammadmahdi.com](mailto:git@mohammadmahdi.com). Do not
open a public issue for a security report.

Supported versions: the latest 1.x release receives security fixes. Include the
affected version, a minimal reproduction, and the platform and compiler used.
The queue is header-only; most issues reproduce in a small program that
instantiates the affected type.

## Automated analysis

CodeQL analyzes the C++ headers as instantiated by the tests and examples, the
Python tooling, and the GitHub Actions workflows on pushes and pull requests to
`main`, plus a weekly scheduled scan. The action and analysis bundle are pinned
in [`ci/toolchain.lock`](ci/toolchain.lock). Results are visible to maintainers
in the repository's
[Security tab](https://github.com/mmabdpr/mpmc-queue/security). CodeQL does not
model C++ memory ordering; it complements, but does not
replace, the TSan tests, stress tests, or the bounded TLA+ model.

## Shared memory

`shared_memory_queue` is intended for trusted, cooperating processes on the same
host. It validates the mapping header, sizes, alignment, endianness, ABI tag,
and instance ID, but it cannot protect against a malicious process with write
access to the segment. Use operating-system permissions and process isolation
for untrusted peers.
