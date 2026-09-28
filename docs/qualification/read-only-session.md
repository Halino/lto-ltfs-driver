# Read-only session enforcement

Status on 2026-09-08: candidate source only, not packaged or deployed. The normal
hardware-free suite and ASan/UBSan suite each passed all 35 groups with zero
failures or skips. LeakSanitizer is disabled by the existing qualification recipe.
All 92 release19 packaging/provenance tests also passed without skips, including the exact
public upstream Git-tree comparison. Binary RPM construction, deployment and
physical validation remain pending.

The upstream comparison used tag `v2.4.8.4-10522`, commit
`7d0de7c0a71296353160f4c5bc082fec9af04e5c`, tree
`5e22b0e576d2cb15deb41bdee1073f862c5b48e0`. This verifies the imported source
record against the public tree; it does not recover the lost private Git history
or alter the historical local-source-baseline record.

## Defect and policy

The forced-read-only flag is bit 32 (`0x100000000`). Its previous 32-bit storage
discarded that bit, and the media-load refresh also cleared session policy.
Consequently, a FUSE read-only mount alone did not prevent every native
mount-time metadata write. The earlier successful sample readback proves the
sampled file hashes, not the absence of all MAM writes.

The candidate preserves the flag in a 64-bit field and applies it before device
open/load and mount-time consistency processing. Device reopen retains the same
device object; load refresh preserves the session bit while refreshing hardware
protection. Every consumer of the changed internal structure must be rebuilt.

Forced-read-only sessions reject payload writes, filemarks, MAM updates and index
writes, including the write-error recovery entry point. Consistency inspection
cannot repair the medium. High-level coherency refresh is skipped before reading
the volume-change reference or changing cached coherency data.

Unmount skips index commits even if the latest index resides on the data
partition or access-time updates have dirtied the in-memory index. It does not
clear those flags or increment the generation. Periodic commit initialization
is a successful no-op for forced-read-only volumes. Writable sessions retain
their normal scheduler and MAM finalization behavior after a write error.

## Hardware-free evidence and limits

Real C entry points are exercised against fake backend callbacks. Regression
tests reproduced failures before the corresponding fixes and now cover forced
payload/filemark rejection, both MAM write entries, repeated load refresh,
coherency refresh, direct index repair, three unmount cases and read-only versus
writable periodic scheduler initialization. Existing writable coherency
serialization and failure propagation tests remain active.

The verified surface contains 136 options and 606 exports: 56 source-call probes
and 550 ABI-only probes. ABI presence is not functional coverage of every export.
The tests run with tape/SG devices inaccessible and a private temporary filesystem.
Eight initial failures came from hard-coded temporary-file paths under a
read-only `/tmp`; those passed with a private writable tmpfs. A stale expected
surface count was aligned without relaxing the coverage verifier.

No installed package, backup job or tape was changed by these tests. This is not
a physical-media interoperability result, nor proof that previous sessions never
modified MAM. Physical qualification must use the reviewed maintenance exclusion
and identity checks after the release gates pass.
