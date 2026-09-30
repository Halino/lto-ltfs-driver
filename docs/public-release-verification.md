# Public driver release: approval and verification

This source tree is a **candidate**, not evidence that a GitHub Release exists.
The public driver repository is
[Halino/lto-ltfs-driver](https://github.com/Halino/lto-ltfs-driver); the paired
application lives on [Halino/lto-archiver's Linux branch](https://github.com/Halino/lto-archiver/tree/linux).
The local `0.1.1-22` / `v0.1.1` identity is a proposed candidate; its source
transfer, signed RPM release and qualification remain pending.
Publication requires separate source/tag approval and separate final-asset
approval. Neither a successful local build nor an approved workflow is permission
to push source, import a signing key, publish assets, or install on a host.

The exact driver package is `lto-ltfs-0.1.1-22.el9.x86_64.rpm`; the matching
source package is `lto-ltfs-0.1.1-22.el9.src.rpm`. The workflow builds twice on
GitHub-hosted runners using the same digest-pinned UBI 9 image and an exact
public dependency lock. It compares unsigned bytes before protected signing,
then runs a provider-free, no physical tape install/fixture/erase check. The
separately approved no-tape report must identify the successful build run.
The official CentOS `icu` RPM supplies isolated **build tools only**; its
executables and libraries are excluded from the distributed RPM and SRPM.

The Release is a direct-download set, **no DNF repository**. It must contain
exactly the two RPMs, `lto-ltfs-0.1.1.tar.gz`, `SOURCE-MANIFEST.json`,
`BUILD-INPUTS.json`, `RPM-PAYLOAD-DIGEST`, `FINAL-RPM-SHA256SUMS`,
`FINAL-RPM-SHA256SUMS.asc`, `RPM-PUBLIC-KEY.asc`, and `ATTESTATION.json`.
Every filename and SHA-256, the full primary and signing-subkey fingerprints,
repository, tag, commit and build run must be reviewed as one set before the
draft is created. Keep that approved JSON outside the source checkout.

After a Release is actually published, an operator can download the assets and
verify them against the separately recorded exact approval:

```bash
gh release download TAG --repo Halino/lto-ltfs-driver --dir downloaded --pattern '*'
(cd downloaded && sha256sum -c FINAL-RPM-SHA256SUMS)
gpg --verify downloaded/FINAL-RPM-SHA256SUMS.asc downloaded/FINAL-RPM-SHA256SUMS
rpm -K downloaded/*.rpm
gh attestation verify downloaded/lto-ltfs-0.1.1-22.el9.x86_64.rpm \
  --repo Halino/lto-ltfs-driver --signer-workflow Halino/lto-ltfs-driver/.github/workflows/build-release.yml \
  --source-ref refs/tags/TAG --source-digest REVIEWED_COMMIT \
  --signer-digest REVIEWED_COMMIT --deny-self-hosted-runners \
  --bundle downloaded/ATTESTATION.json
python3 scripts/verify-public-release.py \
  --candidate downloaded --approved-json APPROVED.json \
  --source-root CLEAN_TAG_CHECKOUT --report verification.json
```

Run the same attestation check for the SRPM. The complete verifier requires
exactly the approved assets, an isolated RPM keyring, full GPG fingerprint and
signature checks, the source tar matching the clean tag, the SRPM matching
`SOURCE-MANIFEST.json`, installed notices and provenance inventory matching
tagged source, and the attested workflow/tag/commit. `rpm -K` alone is not an
approval substitute. Do not supply an unreviewed key or trust a Release page
merely because it displays a green badge.

The publication workflow first checks GitHub's repository-level immutable
release setting with an Administration-read credential. It checks again after
verifying the exact numeric draft, then requires a bound proof no more than
120 seconds old immediately before publication. The finalizer has no second
approval Environment or Administration credential. Those checks reduce the
window but the GitHub setting read and `draft=false` write are **non-atomic**:
an administrator could change the setting between them. The separate final
approval must explicitly acknowledge this residual race. Post-publication
`isImmutable: true` is evidence of the final state, not proof that no window
ever existed.

Licensing is documented in [LICENSE](../LICENSE), [NOTICES](../NOTICES),
[COPYING.LIB](../COPYING.LIB), [LGPL-NOTICE](../LGPL-NOTICE) and
[the per-file inventory](../provenance/license-inventory.json). Seven downstream
origins remain **conditional and unverified**. The conservative
**LGPL-2.1-only** label is the owner's selected policy, not a claim that
provenance has been independently established. Preserve every notice; an
origin or licensing claim remains a separate issue to resolve if raised.

No physical tape operation, vendor hardware qualification, application
upgrade, catalog migration, or production installation follows from the
hardware-free smoke result. For a new application installation, use the
separately released and verified driver first, then the Python runtime, then
the application; the application project's exact signed-input and fresh-host
gates must pass before any package operation.
