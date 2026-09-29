#!/usr/bin/env python3
"""Fail-closed driver Release admission primitives (no publication side effects)."""

from __future__ import annotations

import hashlib
import json
import re
from collections.abc import Mapping


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
_REPO = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")
_TAG = re.compile(r"v[0-9]+\.[0-9]+\.[0-9]+\Z")


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
