#!/usr/bin/env python3
"""Mechanical, fail-closed checks for a manually reviewed public license inventory.

This cannot establish authorship or whether a change derives from HPE code.
"""

import argparse
import hashlib
import json
import re
import sys
import tarfile
from pathlib import Path, PurePosixPath


SPDX = re.compile(r"SPDX-License-Identifier:[ \t]*([^\r\n*]+)")
LICENSE_ID = re.compile(r"[A-Za-z0-9.+-]+")
OWNER_POLICY_BASIS = "owner-selected conservative LGPL-2.1-only policy"


def fail(message):
    raise ValueError(message)


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def safe_source(root, name):
    candidate = PurePosixPath(name)
    if (
        candidate.is_absolute()
        or not name
        or "\\" in name
        or any(part in ("", ".", "..") for part in candidate.parts)
        or str(candidate) != name
    ):
        fail(f"unsafe source path: {name!r}")
    path = root.joinpath(*candidate.parts)
    if root.is_symlink() or any(
        root.joinpath(*candidate.parts[:index]).is_symlink()
        for index in range(1, len(candidate.parts) + 1)
    ):
        fail(f"linked source: {name}")
    if not path.is_file():
        fail(f"missing or linked source: {name}")
    return path


def rows(path, label):
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list) or any(not isinstance(row, dict) for row in data):
        fail(f"{label} must be a JSON list of objects")
    return data


def conjunctive_license_ids(expression, label):
    """Accept only explicit intersections of simple SPDX IDs."""
    if not isinstance(expression, str) or not expression:
        fail(f"{label} license is empty")
    parts = expression.split(" AND ")
    if any(not LICENSE_ID.fullmatch(part) or part in {"AND", "OR", "WITH"}
           for part in parts):
        fail(f"compound SPDX expression requires explicit review: {label}")
    if len(set(parts)) != len(parts):
        fail(f"duplicate license ID: {label}")
    return set(parts)

def verify_archive_tree(root, archive_path, overlay_paths):
    """Bind reviewed source bytes to the archive, without extracting it."""
    seen = set()
    with tarfile.open(archive_path, "r:*") as archive:
        for member in archive:
            name = member.name
            if name.startswith(root.name + "/"):
                name = name[len(root.name) + 1:]
            if not name or name == root.name:
                if not member.isdir():
                    fail("unsafe source archive root")
                continue
            candidate = PurePosixPath(name)
            if (candidate.is_absolute() or "\\" in name or ".." in candidate.parts
                    or str(candidate) != name.rstrip("/")):
                fail(f"unsafe source archive member: {name!r}")
            if member.isdir():
                continue
            if not member.isfile() or name in seen:
                fail(f"linked or duplicate source archive member: {name}")
            seen.add(name)
            source = safe_source(root, name)
            archived = archive.extractfile(member)
            if archived is None:
                fail(f"unreadable source archive member: {name}")
            digest = hashlib.sha256()
            for block in iter(lambda: archived.read(1024 * 1024), b""):
                digest.update(block)
            if digest.hexdigest() != file_digest(source):
                fail(f"archive content differs from reviewed tree: {name}")
    if not set(overlay_paths).issubset(seen):
        fail("source archive omits a reviewed downstream overlay")



def check(args):
    if not re.fullmatch(r"[0-9a-f]{64}", args.source_sha256):
        fail("source archive SHA-256 must be 64 lowercase hex digits")
    if file_digest(args.source_archive) != args.source_sha256:
        fail("source archive SHA-256 mismatch")
    if args.root.is_symlink():
        fail("linked source root")
    args.root = args.root.resolve()

    overlays = rows(args.overlays, "overlays")
    inventory = rows(args.inventory, "inventory")
    overlay_paths = [row.get("path") for row in overlays]
    inventory_paths = [row.get("path") for row in inventory]
    if any(not isinstance(path, str) for path in overlay_paths + inventory_paths):
        fail("every entry requires a string path")
    if len(set(overlay_paths)) != len(overlay_paths):
        fail("duplicate overlay path")
    if len(set(inventory_paths)) != len(inventory_paths):
        fail("duplicate inventory path")
    if set(overlay_paths) != set(inventory_paths):
        fail("inventory must cover every downstream overlay exactly")
    verify_archive_tree(args.root, args.source_archive, overlay_paths)

    package_licenses = conjunctive_license_ids(args.package_license, "package")
    by_path = {row["path"]: row for row in inventory}
    conditional = 0
    for overlay in overlays:
        name = overlay["path"]
        source = safe_source(args.root, name)
        actual = file_digest(source)
        if overlay.get("downstream_sha256") != actual:
            fail(f"overlay SHA-256 mismatch: {name}")
        item = by_path[name]
        if item.get("sha256") != actual:
            fail(f"inventory SHA-256 mismatch: {name}")
        status = item.get("review_status")
        if status == "policy_approved":
            if (
                item.get("origin_certainty") != "unverified"
                or item.get("approval_basis") != OWNER_POLICY_BASIS
                or "LGPL-2.1-only" not in conjunctive_license_ids(
                    item.get("license"), name
                )
            ):
                fail(f"conditional LGPL approval evidence is incomplete: {name}")
            conditional += 1
        elif status != "reviewed":
            fail(f"unreviewed downstream overlay: {name}")
        if not all(isinstance(item.get(key), str) and item[key].strip()
                   for key in ("origin", "evidence", "license")):
            fail(f"incomplete license/origin evidence: {name}")
        identified = item["license"]
        headers = {
            header.strip() for header in SPDX.findall(
                source.read_bytes()[:8192].decode("utf-8", "replace"))
        }
        for header in headers:
            conjunctive_license_ids(header, name)
        if headers and identified not in headers:
            fail(f"license contradicts SPDX header: {name}: {identified} vs {sorted(headers)}")
        missing = conjunctive_license_ids(identified, name) - package_licenses
        if missing:
            fail(f"package license omits {sorted(missing)[0]}: {name}")
    return len(overlays), conditional


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--source-sha256", required=True)
    parser.add_argument("--overlays", type=Path, required=True)
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--package-license", required=True)
    args = parser.parse_args()
    try:
        count, conditional = check(args)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as exc:
        print(f"license inventory rejected: {exc}", file=sys.stderr)
        return 1
    print(
        f"mechanical inventory checks passed for {count} overlays; "
        f"{conditional} conditional origins remain unverified; "
        "authorship remains a separate review"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
