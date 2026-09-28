# Unmounted read-only capacity diagnostic

Status: hardware-free qualification passed on 2026-09-06; packaging, deployment
and physical qualification remain pending.

The private qualification run passed 35/35 normal groups and
35/35 ASan/UBSan groups, with no skipped groups or failures. The separate
packaging/provenance run passed all 90 tests, and the surface verifier covered
136 options and 605 exports (54 source-call probes, 551 ABI-only probes).
LeakSanitizer remains disabled by the existing sanitizer recipe. Existing
compiler warnings, including deprecated `pthread_yield`, remain; this is not a
warning-free build or a physical-media interoperability result.

The first RPM build passed all 35 test groups and SRPM verification but its
binary-payload verifier rejected the diagnostic's new `HPE` inquiry-vendor
field. The corrected policy admits exactly one NUL-delimited `HPE` field only
in `/usr/bin/ltfs-info`; product, secret and other payload restrictions remain
active. A regression run passed all 91 packaging/provenance
tests without skips. The coordinated application suite in that run failed;
this does not qualify an RPM build, deployment or physical diagnostic.

`ltfs-info --json --mode capacity` observes the drive's reported partition
capacity without mounting, formatting, writing, positioning or ejecting media.
It is a diagnostic, not a writable-payload estimator and not a backup operation.
The existing `unmounted` and `pre-format` schema-2 identity outputs are unchanged.

## Required identity and exclusion

The command requires all five explicit expectations, obtained from previously
sealed evidence rather than copied from the cartridge being observed:

```
--expect-drive-serial SERIAL --expect-medium-serial MAM_SERIAL
--expect-label LABEL --expect-uuid LTFS_UUID --expect-generation GENERATION
```

It uses the root-owned configured device pair and the existing exclusive native
device guard. Before capacity collection it checks the drive's VPD serial and
MAM serial, LTFS label, UUID and generation. It repeats the identity checks after
the capacity read on the same open descriptor, with the guard held throughout.
A mismatch, unsupported response, truncated response or close failure produces
a nonzero exit and no successful JSON report. No automatic recovery command,
reservation registration, mode-page change or diagnostic dump is attempted.

For application-managed hardware the native lock is **not** a daemon database
lease. Run only through the qualified maintenance exclusion procedure: preserve
the paused job and prior unit states, fence all application activation paths,
prove no active hardware descriptors or LTFS mounts, run one bounded diagnostic,
then re-prove quiescence before restoring only the owned activation changes.
Do not invoke this tool alongside an active backup, restore or qualification.
Do not reuse a maintenance script carrying obsolete build or authority pins.

## Capacity interpretation

The diagnostic recognizes explicit HP/HPE, IBM and Quantum LTO inquiry product
families from generation 5 onward (through the known product names in this
source). Unknown products are rejected rather than assigned a guessed capacity
profile. Generation support in code does not imply physical qualification of
each drive/media family.

Page selection uses the same predicate as the SG backup backend: page `0x31`
for LTO-5 and HP LTO-6, otherwise page `0x17` for recognized LTO products. Both
paths use parsers shared with that backend. Page `0x31` values are already MiB;
page `0x17` decimal-MB values are converted to MiB. The diagnostic always reports
an explicit capacity offset of zero, independently of any mount configuration.

The separate JSON report has `schema: 1`, `kind: "ltfs-capacity"`,
`identity_verified_before_after: true`, `unit: "MiB"`, numeric `log_page`
(49 or 23), `capacity_offset: 0`, and these integer values:

- `remaining_partition0_mib`, `remaining_partition1_mib`
- `maximum_partition0_mib`, `maximum_partition1_mib`

`identity` contains the unchanged schema-2 LTFS identity object. Partition
numbers are physical partitions, not an assumption that either is the payload
partition. Convert MiB to bytes with 1,048,576, not 1,000,000. Reported partition
capacity is not a promise that that many bytes of user files can be committed.
Filesystem/index overhead and actual write/finalization evidence remain distinct.
No universal percentage reserve is inferred from one cartridge's fill trial.

## Hardware-free evidence

`--self-test-fixture capacity` is synthetic and opens no device. The C tests
exercise identity continuity, all collector failure boundaries, one-open/close
ownership, strict page parsing, unit conversion and actual SG request formation
with ioctl replaced at the kernel boundary. Short transfers and SCSI errors
must fail without issuing recovery or mutating commands. Python tests preserve
the existing schema-2 contracts and reject incomplete diagnostic expectations.

These tests do not prove the capacity or readability of an actual cartridge.
Physical validation and sample readback of previously written backup tapes are
separate gates, and must not be reported complete from these fixtures.
