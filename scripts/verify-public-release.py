#!/usr/bin/env python3
"""Fail-closed driver Release admission primitives (no publication side effects)."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import runpy
import stat
import subprocess
import sys
import tempfile
from collections.abc import Mapping
from pathlib import Path


class PublicDriverReleaseError(RuntimeError):
    """The proposed public driver Release has not passed admission."""


# Exact driver-only assets; app/runtime files are not part of this Release.
RELEASE_ASSET_NAMES = frozenset({
    "lto-ltfs-0.1.0-22.el9.x86_64.rpm",
    "lto-ltfs-0.1.0-22.el9.src.rpm",
    "lto-ltfs-0.1.0.tar.gz",
    "SOURCE-MANIFEST.json",
    "BUILD-INPUTS.json",
    "RPM-PAYLOAD-DIGEST",
    "FINAL-RPM-SHA256SUMS",
    "FINAL-RPM-SHA256SUMS.asc",
    "RPM-PUBLIC-KEY.asc",
    "ATTESTATION.json",
})
PROOF_IDENTITY_KEYS = frozenset({
    "repo", "tag", "commit", "draft_id", "manifest_sha256",
    "asset_set_sha256", "run_id",
})
_SHA = re.compile(r"[0-9a-f]{64}\Z")
_COMMIT = re.compile(r"[0-9a-f]{40}\Z")
_FPR = re.compile(r"[0-9A-F]{40}\Z")
_REPO = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")
_TAG = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+\Z")
_BINARY = "lto-ltfs-0.1.0-22.el9.x86_64.rpm"
_SRPM = "lto-ltfs-0.1.0-22.el9.src.rpm"


def asset_set_sha256(files: Mapping[str, str]) -> str:
    """Hash sorted name-NUL-lowercase-SHA-LF rows for the exact asset set."""
    if not isinstance(files, Mapping):
        raise PublicDriverReleaseError("invalid approved driver asset set")
    rows = list(files.items())
    if (
        len(rows) != len(RELEASE_ASSET_NAMES)
        or {name for name, _digest in rows} != RELEASE_ASSET_NAMES
        or len({name for name, _digest in rows}) != len(rows)
        or any(
            type(name) is not str or type(digest) is not str
            or _SHA.fullmatch(digest) is None for name, digest in rows
        )
    ):
        raise PublicDriverReleaseError("approved driver asset closure differs")
    encoded = b"".join(
        name.encode("ascii") + b"\0" + digest.encode("ascii") + b"\n"
        for name, digest in sorted(rows)
    )
    return hashlib.sha256(encoded).hexdigest()


def _sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify_release_files(candidate: Path, approved: Mapping[str, str]) -> None:
    """Admit exact ten regular assets, then bind the final two RPM hashes."""
    if (
        not isinstance(approved, Mapping)
        or len(approved) != len(RELEASE_ASSET_NAMES)
        or set(approved) != RELEASE_ASSET_NAMES
        or any(type(value) is not str or _SHA.fullmatch(value) is None
               for value in approved.values())
        or candidate.is_symlink() or not candidate.is_dir()
    ):
        raise PublicDriverReleaseError("signed driver candidate identity differs")
    members = list(candidate.iterdir())
    if {member.name for member in members} != RELEASE_ASSET_NAMES:
        raise PublicDriverReleaseError("signed driver asset closure differs")
    for member in members:
        status = member.lstat()
        if (
            member.is_symlink() or not stat.S_ISREG(status.st_mode)
            or status.st_nlink != 1 or _sha256(member) != approved[member.name]
        ):
            raise PublicDriverReleaseError("signed driver asset bytes differ from approval")
    rpm_names = sorted(name for name in RELEASE_ASSET_NAMES if name.endswith(".rpm"))
    expected_manifest = "".join(f"{approved[name]}  {name}\n" for name in rpm_names)
    try:
        manifest = (candidate / "FINAL-RPM-SHA256SUMS").read_text(encoding="ascii")
    except (OSError, UnicodeError) as error:
        raise PublicDriverReleaseError("final driver manifest is unreadable") from error
    if manifest != expected_manifest:
        raise PublicDriverReleaseError("final driver manifest differs from approved RPM bytes")


def attestation_command(
    rpm: Path, repo: str, signer_workflow: str, source_ref: str,
    commit: str, bundle: Path,
) -> list[str]:
    """Require one tag/commit-bound GitHub Actions attestation per signed RPM."""
    if (
        _REPO.fullmatch(repo) is None
        or signer_workflow != f"{repo}/.github/workflows/build-release.yml"
        or re.fullmatch(r"refs/tags/v[0-9]+\.[0-9]+\.[0-9]+", source_ref) is None
        or _COMMIT.fullmatch(commit) is None
        or not str(rpm).endswith(".rpm") or bundle.name != "ATTESTATION.json"
    ):
        raise PublicDriverReleaseError("invalid driver attestation identity")
    return [
        "gh", "attestation", "verify", str(rpm),
        "--repo", repo, "--signer-workflow", signer_workflow,
        "--source-ref", source_ref, "--source-digest", commit,
        "--signer-digest", commit, "--deny-self-hosted-runners",
        "--bundle", str(bundle),
    ]


def verify_signature_output(output: bytes, fingerprint: str) -> None:
    if _FPR.fullmatch(fingerprint) is None:
        raise PublicDriverReleaseError("invalid driver signing fingerprint")
    normalized = output.decode("ascii", errors="replace").lower()
    if (
        "rsa/sha256 signature" not in normalized
        or f"key id {fingerprint[-8:].lower()}: ok" not in normalized
        or "not ok" in normalized or "nokey" in normalized
    ):
        raise PublicDriverReleaseError("driver RPM signature differs from approved subkey")


def verify_gpg_status(output: bytes, signing_subkey: str) -> None:
    if _FPR.fullmatch(signing_subkey) is None:
        raise PublicDriverReleaseError("invalid approved driver signing subkey")
    found = []
    for line in output.decode("ascii", errors="replace").splitlines():
        fields = line.split()
        if fields[:2] == ["[GNUPG:]", "VALIDSIG"] and len(fields) >= 3:
            found.append(fields[2].upper())
    if found != [signing_subkey]:
        raise PublicDriverReleaseError("driver manifest uses a different signing subkey")


def verify_public_key_records(output: bytes, primary: str, subkey: str) -> None:
    """Bind the approved signing subkey to exactly one approved primary key."""
    if (
        _FPR.fullmatch(primary) is None or _FPR.fullmatch(subkey) is None
        or primary == subkey
    ):
        raise PublicDriverReleaseError("invalid approved driver key fingerprints")
    try:
        records = [line.split(":") for line in output.decode("ascii").splitlines()]
    except UnicodeError as error:
        raise PublicDriverReleaseError("invalid driver public key listing") from error
    primary_count = 0
    primary_fprs: list[str] = []
    subkey_fprs: list[str] = []
    awaiting: str | None = None
    for fields in records:
        kind = fields[0]
        if kind == "pub":
            primary_count += 1
            awaiting = "pub"
            if len(fields) < 2 or fields[1].lower() in {"r", "e", "d", "i"}:
                raise PublicDriverReleaseError("unusable driver public key")
        elif kind == "sub":
            if primary_count != 1 or len(fields) < 2 or fields[1].lower() in {"r", "e", "d", "i"}:
                raise PublicDriverReleaseError("unusable driver signing subkey")
            awaiting = "sub"
        elif kind == "fpr" and awaiting:
            (primary_fprs if awaiting == "pub" else subkey_fprs).append(
                fields[9] if len(fields) > 9 else ""
            )
            awaiting = None
    if primary_count != 1 or primary_fprs != [primary] or subkey_fprs.count(subkey) != 1:
        raise PublicDriverReleaseError("driver public key differs from full approval")


def _run(command: list[str]) -> bytes:
    try:
        return subprocess.run(command, check=True, capture_output=True, timeout=240).stdout
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise PublicDriverReleaseError(f"driver verification command failed: {command[0]}") from error


def verify_source_inputs(candidate: Path, source_root: Path, tag: str, commit: str) -> None:
    """Link source tar, source manifest, SRPM and installed notices to one tag."""
    if (
        _TAG.fullmatch(tag) is None or _COMMIT.fullmatch(commit) is None
        or source_root.is_symlink() or not source_root.is_dir()
    ):
        raise PublicDriverReleaseError("invalid reviewed driver source identity")
    for arguments in (
        ["rev-parse", "HEAD"], ["rev-parse", f"refs/tags/{tag}^{{commit}}"],
    ):
        actual = _run(["git", "-C", str(source_root), *arguments]).decode("ascii").strip()
        if actual != commit:
            raise PublicDriverReleaseError("driver tag or checkout differs from approved commit")
    if _run(["git", "-C", str(source_root), "status", "--porcelain=v1", "--untracked-files=all"]).strip():
        raise PublicDriverReleaseError("reviewed driver tag checkout is dirty")
    try:
        builder = runpy.run_path(str(source_root / "scripts/verify-public-build.py"))
        inspector = runpy.run_path(str(source_root / "scripts/verify-rpm.py"))
        source_archive = candidate / "lto-ltfs-0.1.0.tar.gz"
        manifest = candidate / "SOURCE-MANIFEST.json"
        builder["verify_tag_archive"](source_root, commit, source_archive)
        inspector["verify_source_manifest"](source_archive, manifest)
        inspector["verify_source_rpm"](candidate / _SRPM, manifest)
        inspector["verify"](candidate / _BINARY)
        icu = runpy.run_path(str(source_root / "scripts/prepare-icu-build-tools.py"))
        lock = icu["load_icu_lock"](source_root / "packaging/rpm/icu-build-tools.json")
        if (candidate / "BUILD-INPUTS.json").read_bytes() != builder["build_inputs_bytes"](lock):
            raise PublicDriverReleaseError("driver build inputs differ from reviewed lock")
        payload_digest = _run([
            "rpm", "-qp", "--qf", "%{PAYLOADDIGESTALGO}:%{PAYLOADDIGEST}\n",
            str(candidate / _BINARY),
        ])
        if (candidate / "RPM-PAYLOAD-DIGEST").read_bytes() != payload_digest:
            raise PublicDriverReleaseError("driver payload digest differs from RPM")
        with tempfile.TemporaryDirectory(prefix="lto-driver-release-notices-") as raw:
            extracted = Path(raw)
            records = inspector["read_file_records"](candidate / _BINARY)
            inspector["extract_payload"](candidate / _BINARY, extracted, records)
            builder["verify_installed_notices"](source_root, extracted)
            builder["reject_provider_files"](extracted, builder["_provider_digests"](lock))
    except (OSError, UnicodeError, ValueError) as error:
        raise PublicDriverReleaseError("driver SRPM, payload or tag source refused") from error


def verify_release(
    candidate: Path, approved: Mapping[str, str], primary_fingerprint: str,
    signing_subkey_fingerprint: str, repo: str, tag: str, commit: str,
    source_root: Path,
) -> dict[str, object]:
    """Read-only final verification of the exact signed driver candidate."""
    verify_release_files(candidate, approved)
    if (
        _REPO.fullmatch(repo) is None or _TAG.fullmatch(tag) is None
        or _COMMIT.fullmatch(commit) is None
        or _FPR.fullmatch(primary_fingerprint) is None
        or _FPR.fullmatch(signing_subkey_fingerprint) is None
    ):
        raise PublicDriverReleaseError("invalid approved driver publication identity")
    verify_source_inputs(candidate, source_root, tag, commit)
    public_key = candidate / "RPM-PUBLIC-KEY.asc"
    verify_public_key_records(
        _run(["gpg", "--batch", "--with-colons", "--show-keys", str(public_key)]),
        primary_fingerprint, signing_subkey_fingerprint,
    )
    with tempfile.TemporaryDirectory(prefix="lto-driver-release-rpmdb-") as raw:
        scratch = Path(raw)
        gpg_home = scratch / "gnupg"
        rpmdb = scratch / "rpmdb"
        gpg_home.mkdir(mode=0o700)
        rpmdb.mkdir(mode=0o700)
        _run(["gpg", "--homedir", str(gpg_home), "--batch", "--import", str(public_key)])
        verify_gpg_status(
            _run([
                "gpg", "--homedir", str(gpg_home), "--batch", "--status-fd", "1",
                "--verify", str(candidate / "FINAL-RPM-SHA256SUMS.asc"),
                str(candidate / "FINAL-RPM-SHA256SUMS"),
            ]),
            signing_subkey_fingerprint,
        )
        _run(["rpm", "--dbpath", str(rpmdb), "--initdb"])
        _run(["rpmkeys", "--dbpath", str(rpmdb), "--import", str(public_key)])
        for name in (_SRPM, _BINARY):
            rpm = candidate / name
            verify_signature_output(
                _run(["rpmkeys", "--dbpath", str(rpmdb), "--checksig", "--verbose", str(rpm)]),
                signing_subkey_fingerprint,
            )
            _run(["rpm", "--dbpath", str(rpmdb), "-K", str(rpm)])
            _run(attestation_command(
                rpm, repo, f"{repo}/.github/workflows/build-release.yml",
                f"refs/tags/{tag}", commit, candidate / "ATTESTATION.json",
            ))
    return {
        "schema_version": 1,
        "status": "signed_driver_candidate_verified",
        "repo": repo, "tag": tag, "commit": commit,
        "primary_fingerprint": primary_fingerprint,
        "signing_subkey_fingerprint": signing_subkey_fingerprint,
        "asset_set_sha256": asset_set_sha256(approved),
        "approved_assets_sha256": dict(sorted(approved.items())),
    }


def _unique_pairs(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PublicDriverReleaseError("duplicate driver approval JSON key")
        result[key] = value
    return result


def parse_approval_document(raw: bytes) -> dict[str, object]:
    """Admit the separately reviewed exact release identity, with no extras."""
    if not isinstance(raw, bytes) or not 0 < len(raw) <= 16 * 1024:
        raise PublicDriverReleaseError("driver approval document missing or oversized")
    try:
        data = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_pairs)
    except (UnicodeError, ValueError) as error:
        raise PublicDriverReleaseError("driver approval JSON is malformed") from error
    required = {
        "schema_version", "repo", "tag", "commit", "primary_fingerprint",
        "signing_subkey_fingerprint", "assets",
    }
    if (
        type(data) is not dict or set(data) != required
        or type(data["schema_version"]) is not int or data["schema_version"] != 1
        or type(data["repo"]) is not str or _REPO.fullmatch(data["repo"]) is None
        or type(data["tag"]) is not str or _TAG.fullmatch(data["tag"]) is None
        or type(data["commit"]) is not str or _COMMIT.fullmatch(data["commit"]) is None
        or type(data["primary_fingerprint"]) is not str
        or _FPR.fullmatch(data["primary_fingerprint"]) is None
        or type(data["signing_subkey_fingerprint"]) is not str
        or _FPR.fullmatch(data["signing_subkey_fingerprint"]) is None
    ):
        raise PublicDriverReleaseError("driver approval identity differs from schema")
    asset_set_sha256(data["assets"])
    return data


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv[1:]
    if len(argv) == 2 and argv[0] == "--verify-immutability-http":
        try:
            verify_immutability_response(Path(argv[1]).read_bytes())
            return 0
        except (OSError, PublicDriverReleaseError) as error:
            print(f"driver immutability preflight refused: {error}", file=sys.stderr)
            return 2
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--approved-json", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        status = args.approved_json.lstat()
        if args.approved_json.is_symlink() or not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
            raise PublicDriverReleaseError("unsafe driver approval document")
        approval = parse_approval_document(args.approved_json.read_bytes())
        result = verify_release(
            args.candidate, approval["assets"], approval["primary_fingerprint"],
            approval["signing_subkey_fingerprint"], approval["repo"],
            approval["tag"], approval["commit"], args.source_root,
        )
        with args.report.open("x", encoding="utf-8") as output:
            json.dump(result, output, indent=2, sort_keys=True)
            output.write("\n")
        return 0
    except (OSError, UnicodeError, ValueError, PublicDriverReleaseError) as error:
        print(f"signed driver release refused: {error}", file=sys.stderr)
        return 2


def validate_final_proof(
    proof: dict[str, object], expected: dict[str, object], now_epoch: int
) -> None:
    """Accept only one matching numeric draft and a proof aged 0..120 seconds."""
    if (
        type(proof) is not dict or type(expected) is not dict
        or set(proof) != PROOF_IDENTITY_KEYS | {"checked_at"}
        or set(expected) != PROOF_IDENTITY_KEYS
        or type(now_epoch) is not int or now_epoch < 0
    ):
        raise PublicDriverReleaseError("malformed final driver proof")
    for identity in (proof, expected):
        if (
            type(identity["repo"]) is not str or _REPO.fullmatch(identity["repo"]) is None
            or type(identity["tag"]) is not str or _TAG.fullmatch(identity["tag"]) is None
            or type(identity["commit"]) is not str or _COMMIT.fullmatch(identity["commit"]) is None
            or any(
                type(identity[key]) is not str or _SHA.fullmatch(identity[key]) is None
                for key in ("manifest_sha256", "asset_set_sha256")
            )
            or any(type(identity[key]) is not int or identity[key] <= 0
                   for key in ("draft_id", "run_id"))
        ):
            raise PublicDriverReleaseError("invalid final driver proof identity")
    if any(proof[key] != expected[key] for key in PROOF_IDENTITY_KEYS):
        raise PublicDriverReleaseError("final driver proof differs from approved draft")
    checked_at = proof["checked_at"]
    if type(checked_at) is not int or not 0 <= now_epoch - checked_at <= 120:
        raise PublicDriverReleaseError("final driver proof is stale or from the future")


def make_final_proof(
    repo: str, tag: str, commit: str, draft_id: int, manifest_sha256: str,
    asset_set_sha256: str, run_id: int, checked_at: int,
) -> dict[str, object]:
    proof: dict[str, object] = {
        "repo": repo, "tag": tag, "commit": commit, "draft_id": draft_id,
        "manifest_sha256": manifest_sha256,
        "asset_set_sha256": asset_set_sha256,
        "run_id": run_id, "checked_at": checked_at,
    }
    validate_final_proof(proof, {key: proof[key] for key in PROOF_IDENTITY_KEYS}, checked_at)
    return proof


def verify_immutability_response(response: bytes) -> None:
    """Require authenticated Administration-read HTTP 200 and enabled=true."""
    if not isinstance(response, bytes) or len(response) > 16 * 1024:
        raise PublicDriverReleaseError("immutable-release response missing or oversized")
    normalized = response.replace(b"\r\n", b"\n")
    try:
        headers, body = normalized.split(b"\n\n", 1)
        first = headers.split(b"\n", 1)[0]
        if re.fullmatch(rb"HTTP/[0-9](?:\.[0-9])? 200(?: [^\n]*)?", first) is None:
            raise PublicDriverReleaseError("immutable releases not confirmed with HTTP 200")
        if not any(
            re.fullmatch(
                rb"(?i)content-type: application/(?:json|vnd\.github\+json)(?:;[^\n]*)?",
                line,
            ) for line in headers.split(b"\n")[1:]
        ):
            raise PublicDriverReleaseError("immutable-release response is not JSON")
        state = json.loads(body.decode("utf-8"))
    except (ValueError, UnicodeError) as error:
        raise PublicDriverReleaseError("immutable-release response malformed") from error
    if not isinstance(state, dict) or state.get("enabled") is not True:
        raise PublicDriverReleaseError("immutable releases are not enabled")


if __name__ == "__main__":
    raise SystemExit(main())
