#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify the pinned, offline uthash build dependency."""

import hashlib
import json
import sys
from pathlib import Path


EXPECTED = {
    "repository": "https://github.com/troydhanson/uthash.git",
    "commit": "2031adfd8cd6f8f498e0f4a9055648b19496f12e",
    "license": "BSD-1-Clause",
    "files": {
        "src/libltfs/uthash_submodule/LICENSE": (
            "3fbbead84dff6db076bdd60eee3c98dadc35a4a415c72b0a35f95d723931c18a"
        ),
        "src/libltfs/uthash_submodule/src/uthash.h": (
            "61bf8a411bcd81f6e67fe8281001a444ebae51560b3df15ff13bb018fe51f18c"
        ),
    },
}


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def check(root):
    metadata_path = root / "provenance/uthash.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata != EXPECTED:
        raise ValueError("vendored uthash metadata differs from the pinned record")

    for relative_path, expected_hash in EXPECTED["files"].items():
        source_path = root / relative_path
        if not source_path.is_file() or source_path.is_symlink():
            raise ValueError("vendored uthash file is missing or unsafe: " + relative_path)
        if file_sha256(source_path) != expected_hash:
            raise ValueError("vendored uthash file hash differs: " + relative_path)

    license_text = (root / "src/libltfs/uthash_submodule/LICENSE").read_text(
        encoding="utf-8"
    )
    if not license_text.startswith("Copyright (c) 2005-2025, Troy D. Hanson"):
        raise ValueError("vendored uthash license attribution differs")
    if "Redistribution and use in source and binary forms" not in license_text:
        raise ValueError("vendored uthash BSD-1-Clause grant is missing")


def main(argv):
    if len(argv) != 2:
        raise ValueError("usage: check-vendored-uthash.py ROOT")
    root = Path(argv[1]).resolve()
    if not root.is_dir():
        raise ValueError("ROOT is not a directory")
    check(root)


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("vendored uthash check failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
