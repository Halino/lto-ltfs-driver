# SG logical-block-protection response bounds

The Linux SG backend previously sent its entire48-byte allocation in the
close-time MODE SELECT even when MODE SENSE returned only24 bytes. A hardware
trace from an HP Ultrium6-SCSI drive showed a24-byte response followed by a
48-byte request containing unreturned bytes. The drive rejected the parameter
list at byte24. This was a driver command-construction defect; the native LTFS
session still unmounted with its index generation unchanged.

The candidate correction validates the transferred length, MODE SENSE header,
block-descriptor offset and requested mode-page identity/length before reading
or updating fields. Only the validated parameter list is sent to MODE SELECT.
Buffers are initialized, and the PS bit is cleared in the outgoing mode page.
Enterprise capability reads receive the same boundary checks. Malformed
responses remain errors; they do not reach MODE SELECT.

Close now applies the same IBM-vendor condition as setup before resetting LBP.
HP and other non-IBM drives retain reservation deregistration, descriptor close,
open-factor accounting and timeout cleanup. IBM setup/reset and existing
best-effort close return semantics are preserved. No media-write, MAM, index,
capacity or backup-layout behavior is changed.

The control-subpage fields are described in the
[T10 SSC-3 data-protection proposal, table x5](https://www.t10.org/ftp/t10/document.07/07-374r2.pdf#page=11).
This reference describes the field layout; it is not an interoperability
certification or a substitute for the actual hardware trace.

## Verification status

The regression harness compiles the actual `_set_lbp()` and `sg_close()` bodies
with typed transport and cleanup substitutes, not rewritten implementations.
The original implementation failed13 of19 initial behavioral cases. The
correction passed those cases both normally and under ASan/UBSan; leak checking
was disabled consistently with the existing qualification policy.
Three additional cases also pass: transport padding beyond the declared length,
PS-bit normalization and failed MODE SELECT with unchanged CRC callbacks and
preserved best-effort close cleanup (22 cases total).

The cases cover the exact24-byte hardware response, a valid48-byte IBM response,
variable block-descriptor offsets, LTO6 RS and LTO7 CRC32C selection, enterprise
capability parsing, malformed/truncated/failed responses and HP/IBM close paths.
The harness requires no tape device or FUSE/ICU development packages. It does
not replace full production compilation, the complete regression suite or a
subsequent hardware check. The first candidate full RHEL build and normal
suite passed 36/36 tests, with no skips. The fail-fast ASan/UBSan run passed
35/36: `test_tape_close` detected an existing unaligned store through
`ltfs_u16tobe(coh_data + 3)`. This was not a tape operation and did not alter
the installed driver or any media.

All six shared endian conversions now use aligned local integers and `memcpy`
instead of dereferencing cast integer pointers into arbitrary byte buffers.
The big-endian wire representation is unchanged. Direct-header tests cover
16-, 32- and 64-bit reads and writes, offsets 0–7, zero/all-ones/mixed/high-bit
vectors and untouched surrounding bytes. Each of the six old helpers failed
its separate fail-fast UBSan reproduction; the corrected helpers passed both
normal and ASan/UBSan runs. The qualification script now enforces fail-fast
UBSan itself, with a regression probe demonstrating that actual undefined
behavior fails even when the caller omits or disables fail-fast options.
The combined RHEL rerun completed on 2026-09-08: full production builds and
both complete suites passed 37/37 tests each, with no skips or failures. The
previously failing `test_tape_close` and new `test_ltfs_endian` both passed
under fail-fast ASan/UBSan. The separately instrumented SG harness also passed
all 22 cases. Expected negative-test LTFS messages are not sanitizer errors.
This run used private devices/network namespaces and did not access media.

The private qualification evidence is retained outside the public source
distribution. This report does not authorize a new hardware operation.
Packaging/provenance updates, coordinated deployment and the subsequent
candidate hardware check remain pending. Application134 and driver20 are
still installed; no new driver release has been installed.

The separate unsupported LOG SENSE health page0x37 diagnostic is not suppressed.
It leaves the optional health value unavailable and is not evidence that backup
payload or LTFS index finalization failed.
