# Complete LTFS qualification report

Use one immutable evidence directory per run. Record literal values in the
restricted report; publish tape serials, server names, addresses, and device
paths only as redacted digests. A blank field is not a pass.

## Report identity

- Report ID:
- Run start/end (UTC):
- Reviewer and independent reviewer:
- Source repository:
- Commit SHA:
- Tree SHA:
- Working tree clean (`yes`/`no`, evidence):
- Source archive SHA-256:
- SRPM SHA-256:
- RPM SHA-256 (one row per RPM):
- Installed RPM NEVRA and payload digest:
- Surface manifest SHA-256:
- Physical plan SHA-256:
- Broker/configuration/policy SHA-256:
- LTFS, `mkltfs`, `ltfsck`, `ltfs-info`, broker, and runner tool hashes:

## Qualification verdicts

Record `PASS`, `FAIL`, `FENCED`, or `NOT RUN` separately.

| Gate | Verdict | First failure or limitation | Evidence artifact |
|---|---|---|---|
| Hermetic/unit |  |  |  |
| Surface ABI enumeration |  |  |  |
| Surface direct source-call coverage |  |  |  |
| ASan/UBSan |  |  |  |
| gcov |  |  |  |
| RHEL package/install/SELinux/systemd |  |  |  |
| Read-only physical |  |  |  |
| Additive physical |  |  |  |
| Destructive physical |  |  |  |

## Commands and outcomes

For every command record the exact argv array, cwd, immutable input hashes,
start/end timestamps, duration, RC, stdout SHA-256, stderr SHA-256, and verdict.
Do not replace a failed or ambiguous attempt with a retry; record any new
authorized attempt as a separate row.

| Stage | Exact argv / cwd | RC | Duration | stdout/stderr SHA-256 | Verdict |
|---|---|---:|---:|---|---|
|  |  |  |  |  |  |

## Skips and limitations

Each skip requires the exact unsupported component, detection evidence, impact,
and reviewer disposition. “Not applicable” without evidence is invalid.

| Test/operation | Reason | Detection evidence | Coverage impact | Disposition |
|---|---|---|---|---|
|  |  |  |  |  |

## Cassette identity authority

- Catalog job ID and sequence:
- DB physical label:
- MAM barcode:
- LTFS index label:
- DB/MAM/index label byte-exact comparison (`MATCH`/`MISMATCH`):
- DB tape serial (restricted value or redacted digest):
- MAM tape serial (restricted value or redacted digest):
- DB/MAM serial byte-exact comparison (`MATCH`/`MISMATCH`):
- Drive serial/WWID/SCSI tuple evidence digest:
- Volume UUID before qualification:
- Index generation before qualification:
- Capacity and write-protect state before qualification:
- Read-only identity receipt SHA-256:

Any label or serial mismatch makes every mutating verdict `FENCED`.

## Per-operation physical evidence

Create one row for every attempted or skipped operation: discovery, load,
read-only mount/read/unmount, additive write, overwrite, rename, truncate,
delete, repair/recovery, format, reformat, short wipe, long wipe, unload, eject,
and final restore. Destructive operations require an independent sealed token.

| Operation | Token/plan digest | Pre-state label/UUID/generation | Exact command + RC + duration | Receipt/event SHA-256 | Post-state label/UUID/generation | Verdict |
|---|---|---|---|---|---|---|
|  |  |  |  |  |  |  |

For data operations attach the deterministic file manifest, byte counts, file
counts, and content SHA-256 before mutation and after read-only remount. For an
ambiguous response record `FENCED`, whether the operation was automatically
retried (`must be no`), and the reconciliation evidence.

## Final cassette state

- Final media state (`mounted`, `unmounted`, `unloaded`, or `ejected`):
- Final DB physical label:
- Final MAM barcode:
- Final LTFS index label:
- Final DB/MAM/index label comparison:
- Final serial comparison (restricted values or redacted digests):
- Final Volume UUID:
- Final Index generation:
- Final deterministic manifest SHA-256:
- Final read-only remount content verification:
- Final clean unmount/unload/eject receipt SHA-256:
- Tape usable and exactly DB-labelled (`yes`/`no`):

## Artifact inventory and sign-off

| Artifact | Artifact SHA-256 | Log SHA-256 | Size | Retention/location |
|---|---|---|---:|---|
|  |  |  |  |  |

- First failing boundary, if any:
- Unresolved risks:
- Reviewer verdict, name, timestamp, signature/digest:
- Independent reviewer verdict, name, timestamp, signature/digest:
