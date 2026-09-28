#!/usr/bin/env python3
"""Generate deterministic synthetic benchmark payloads without user data."""

from __future__ import annotations

import hashlib
import os
import stat
from pathlib import Path


DEFAULT_LARGE_BYTES = 64 * 1024 * 1024
CHUNK_BYTES = 1024 * 1024


def _owned_directory(root: Path) -> None:
    status = root.lstat()
    if not stat.S_ISDIR(status.st_mode) or status.st_uid != os.geteuid():
        raise ValueError("benchmark root must be an owned non-symlink directory")
    if status.st_mode & 0o077:
        raise ValueError("benchmark root must not be accessible by group or other")


def _payload(seed: int, name: str, chunk_index: int, size: int) -> bytes:
    domain = f"lto-ltfs-benchmark-v1:{seed}:{name}:{chunk_index}".encode("ascii")
    return hashlib.shake_256(domain).digest(size)


def generate(root: Path, seed: int, *, large_bytes: int = DEFAULT_LARGE_BYTES) -> list[dict]:
    root = Path(root)
    _owned_directory(root)
    if type(seed) is not int or seed < 0 or seed > (1 << 63) - 1:
        raise ValueError("seed must be a non-negative signed 64-bit integer")
    if type(large_bytes) is not int or large_bytes < CHUNK_BYTES:
        raise ValueError("large workload must be at least one MiB")
    definitions = [
        ("small-1k.bin", 1024),
        ("small-16k.bin", 16 * 1024),
        ("medium-256k.bin", 256 * 1024),
        ("large-4m.bin", 4 * 1024 * 1024),
        ("incompressible-large.bin", large_bytes),
    ]
    manifest = []
    for name, size in definitions:
        destination = root / name
        digest = hashlib.sha256()
        remaining = size
        chunk_index = 0
        with destination.open("xb") as stream:
            while remaining:
                chunk_size = min(remaining, CHUNK_BYTES)
                chunk = _payload(seed, name, chunk_index, chunk_size)
                stream.write(chunk)
                digest.update(chunk)
                remaining -= chunk_size
                chunk_index += 1
        manifest.append({"name": name, "size_bytes": size, "sha256": digest.hexdigest()})
    return manifest


__all__ = ["DEFAULT_LARGE_BYTES", "generate"]
