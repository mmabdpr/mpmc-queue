# Security

Report suspected vulnerabilities privately through
[GitHub Security Advisories](https://github.com/mmabdpr/mpmc-queue/security/advisories/new)
or by email to [git@mohammadmahdi.com](mailto:git@mohammadmahdi.com). Do not
open a public issue for a security report.

Supported versions: the latest 1.x release receives security fixes. Include the
affected version, a minimal reproduction, and the platform and compiler used.
The queue is header-only; most issues reproduce in a small program that
instantiates the affected type.

## Shared memory

`shared_memory_queue` is intended for trusted, cooperating processes on the same
host. It validates the mapping header, sizes, alignment, endianness, ABI tag,
and instance ID, but it cannot protect against a malicious process with write
access to the segment. Use operating-system permissions and process isolation
for untrusted peers.
