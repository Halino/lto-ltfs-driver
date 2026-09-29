#!/usr/bin/env python3
"""Validate LTFS import-record integrity and an optional full Git-tree oracle."""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath


EXPECTED = {
    "repository": "https://github.com/LinearTapeFileSystem/ltfs.git",
    "tag": "v2.4.8.4-10522",
    "commit": "7d0de7c0a71296353160f4c5bc082fec9af04e5c",
    "tree": "5e22b0e576d2cb15deb41bdee1073f862c5b48e0",
    "license": "BSD-3-Clause",
    "excluded_paths": [
        ".github/",
        ".gitmodules",
        ".travis.yml",
        "src/libltfs/uthash_submodule",
    ],
}
PRIVATE_PATH_PARTS = {"private", "hpe", "storeopen"}
MANIFEST_LINE = re.compile(r"^([0-9a-f]{64})  ([^\n]+)$")
SHA256_VALUE = re.compile(r"^[0-9a-f]{64}$")
OVERLAY_KEYS = {
    "path",
    "upstream_sha256",
    "downstream_sha256",
    "rationale",
    "task",
}
IMPORTED_FILES = {
    ".gitattributes",
    ".gitignore",
    "LICENSE",
    "Makefile.am",
    "NOTICES",
    "README.md",
    "autogen.sh",
    "build.sh",
    "configure.ac",
    "ltfs.pc.in",
    "replace_copyright.pl",
    "validate_error_messages.py",
}
IMPORTED_DIRECTORIES = {"conf", "docs", "init.d", "man", "messages", "src"}
GENERATED_IMPORT_FILENAMES = {"Makefile.in"}
VENDORED_EXCLUDED_PREFIXES = {"src/libltfs/uthash_submodule/"}
DOWNSTREAM_WORKFLOW_FILES = {
    ".github/workflows/ci.yml",
    ".github/workflows/build-release.yml",
    ".github/workflows/publish-release.yml",
}


def fail(message):
    raise ValueError(message)


def is_excluded_path(path):
    return any(
        path == excluded.rstrip("/") or path.startswith(excluded)
        for excluded in EXPECTED["excluded_paths"]
    )


def is_safe_import_path(path):
    candidate = PurePosixPath(path)
    if candidate.is_absolute() or ".." in candidate.parts or path.startswith("./"):
        return False
    if any(part.lower() in PRIVATE_PATH_PARTS for part in candidate.parts):
        return False
    return not is_excluded_path(path)


def is_safe_overlay_path(path):
    candidate = PurePosixPath(path)
    if candidate.is_absolute() or ".." in candidate.parts or path.startswith("./"):
        return False
    if any(part.lower() in PRIVATE_PATH_PARTS for part in candidate.parts):
        return False
    return not is_excluded_path(path) or any(
        path.startswith(prefix) for prefix in VENDORED_EXCLUDED_PREFIXES
    )


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_metadata(root):
    metadata_path = root / "provenance" / "upstream.json"
    data = json.loads(metadata_path.read_text(encoding="utf-8"))
    if set(data) != set(EXPECTED):
        fail("upstream metadata keys do not match the provenance schema")
    if data != EXPECTED:
        fail("upstream metadata does not match the pinned import")


def verify_license(root):
    license_text = (root / "LICENSE").read_text(encoding="utf-8")
    if not license_text.startswith("Copyright 2010, 2025 IBM Corp. All rights reserved."):
        fail("LICENSE does not begin with the upstream IBM copyright")
    conditions = (
        "1. Redistributions of source code must retain",
        "2. Redistributions in binary form must reproduce",
        "3. Neither the name of the copyright holder",
    )
    if not all(condition in license_text for condition in conditions):
        fail("LICENSE is missing a BSD-3-Clause redistribution condition")


def read_manifest(root):
    manifest = root / "provenance" / "upstream-files.sha256"
    entries = []
    for line in manifest.read_text(encoding="utf-8").splitlines():
        match = MANIFEST_LINE.fullmatch(line)
        if not match:
            fail("invalid upstream manifest entry")
        digest, relative_path = match.groups()
        if not is_safe_import_path(relative_path):
            fail("unsafe upstream manifest path: " + relative_path)
        entries.append((relative_path, digest))
    if not entries or entries != sorted(entries):
        fail("upstream manifest must be non-empty and sorted by path")
    if len({path for path, _ in entries}) != len(entries):
        fail("upstream manifest contains duplicate paths")
    return dict(entries)


def is_imported_namespace_path(path):
    return path in IMPORTED_FILES or any(
        path.startswith(directory + "/") for directory in IMPORTED_DIRECTORIES
    )


def read_overlays(root):
    overlay_path = root / "provenance/downstream-overlays.json"
    data = json.loads(overlay_path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        fail("downstream overlay schema must be a JSON array")

    entries = []
    for entry in data:
        if not isinstance(entry, dict) or set(entry) != OVERLAY_KEYS:
            fail("downstream overlay entry does not match the schema")
        path = entry["path"]
        upstream_hash = entry["upstream_sha256"]
        downstream_hash = entry["downstream_sha256"]
        if not isinstance(path, str) or not is_safe_overlay_path(path):
            fail("unsafe downstream overlay path: " + str(path))
        if not is_imported_namespace_path(path):
            fail("downstream overlay path is outside the import namespace: " + path)
        if upstream_hash is not None and (
            not isinstance(upstream_hash, str) or not SHA256_VALUE.fullmatch(upstream_hash)
        ):
            fail("invalid downstream overlay upstream hash: " + path)
        if downstream_hash is not None and (
            not isinstance(downstream_hash, str)
            or not SHA256_VALUE.fullmatch(downstream_hash)
        ):
            fail("invalid downstream overlay downstream hash: " + path)
        if not isinstance(entry["rationale"], str) or not entry["rationale"].strip():
            fail("downstream overlay rationale must be non-empty: " + path)
        if not isinstance(entry["task"], str) or not entry["task"].strip():
            fail("downstream overlay task must be non-empty: " + path)
        entries.append(entry)

    paths = [entry["path"] for entry in entries]
    if len(paths) != len(set(paths)):
        fail("downstream overlays contain a duplicate path")
    if paths != sorted(paths):
        fail("downstream overlays must be sorted by path")
    return entries


def expected_current_entries(manifest_entries, overlay_entries):
    expected = dict(manifest_entries)
    for entry in overlay_entries:
        path = entry["path"]
        upstream_hash = entry["upstream_sha256"]
        downstream_hash = entry["downstream_sha256"]
        manifest_hash = manifest_entries.get(path)
        if upstream_hash != manifest_hash:
            fail("downstream overlay upstream hash differs from manifest: " + path)
        if upstream_hash == downstream_hash:
            fail("unnecessary downstream overlay: " + path)
        if downstream_hash is None:
            del expected[path]
        else:
            expected[path] = downstream_hash
    return expected


def imported_namespace_paths(root):
    paths = set()
    for relative_path in IMPORTED_FILES:
        source_path = root / relative_path
        if source_path.is_symlink() or (source_path.exists() and not source_path.is_file()):
            fail("import boundary file is unsafe: " + relative_path)
        if source_path.is_file():
            paths.add(relative_path)
    for directory in IMPORTED_DIRECTORIES:
        source_directory = root / directory
        if source_directory.is_symlink() or (
            source_directory.exists() and not source_directory.is_dir()
        ):
            fail("import boundary directory is unsafe: " + directory)
        if not source_directory.exists():
            continue
        for source_path in source_directory.rglob("*"):
            if source_path.is_symlink():
                fail("import boundary contains a symlink: " + str(source_path.relative_to(root)))
            if source_path.is_file():
                relative_path = source_path.relative_to(root).as_posix()
                if source_path.name in GENERATED_IMPORT_FILENAMES:
                    continue
                paths.add(relative_path)
    return paths


def verify_current_tree(root, entries):
    actual_paths = imported_namespace_paths(root)
    expected_paths = set(entries)
    if actual_paths != expected_paths:
        missing = sorted(expected_paths - actual_paths)
        unexpected = sorted(actual_paths - expected_paths)
        details = []
        if missing:
            details.append("missing=" + ", ".join(missing[:3]))
        if unexpected:
            details.append("unexpected=" + ", ".join(unexpected[:3]))
        fail("import boundary differs from manifest: " + "; ".join(details))
    for relative_path, expected_digest in entries.items():
        source_path = root / relative_path
        if sha256(source_path) != expected_digest:
            fail("downstream hash differs from declared tree: " + relative_path)
    for excluded in EXPECTED["excluded_paths"]:
        excluded_path = excluded.rstrip("/")
        if excluded_path == ".github" and (root / excluded_path).exists():
            github = root / excluded_path
            members = list(github.rglob("*"))
            files = {
                path.relative_to(root).as_posix()
                for path in members if path.is_file() and not path.is_symlink()
            }
            directories = {
                path.relative_to(root).as_posix()
                for path in members if path.is_dir() and not path.is_symlink()
            }
            if (
                github.is_symlink() or not github.is_dir()
                or not files or not files <= DOWNSTREAM_WORKFLOW_FILES
                or directories != {".github/workflows"}
                or any(path.is_symlink() or not (path.is_file() or path.is_dir())
                       for path in members)
            ):
                fail("excluded upstream path is present: " + excluded)
            continue
        declared = any(
            path == excluded_path or path.startswith(excluded_path + "/")
            for path in entries
        )
        if (root / excluded_path).exists() and not declared:
            fail("excluded upstream path is present: " + excluded)


def git_output(upstream, *arguments):
    result = subprocess.run(
        ["git", "-C", str(upstream), *arguments],
        check=True,
        capture_output=True,
    )
    return result.stdout


def upstream_tree_entries(upstream, metadata):
    commit = metadata["commit"]
    tag_ref = "refs/tags/" + metadata["tag"] + "^{commit}"
    try:
        resolved_tag = git_output(upstream, "rev-parse", tag_ref).decode().strip()
    except subprocess.CalledProcessError:
        fail("upstream pinned tag is absent: " + metadata["tag"])
    if resolved_tag != commit:
        fail("upstream pinned tag does not resolve to the pinned commit: " + metadata["tag"])
    resolved_commit = git_output(upstream, "rev-parse", commit + "^{commit}").decode().strip()
    if resolved_commit != commit:
        fail("upstream checkout does not contain the pinned commit")
    resolved_tree = git_output(upstream, "rev-parse", commit + "^{tree}").decode().strip()
    if resolved_tree != metadata["tree"]:
        fail("upstream pinned commit does not have the recorded tree")
    entries = {}
    tree_output = git_output(upstream, "ls-tree", "-r", "-z", "--full-tree", commit)
    for record in tree_output.split(b"\0"):
        if not record:
            continue
        header, encoded_path = record.split(b"\t", 1)
        mode, object_type, _object_id = header.decode().split()
        relative_path = encoded_path.decode()
        if is_excluded_path(relative_path):
            continue
        if object_type != "blob" or mode not in {"100644", "100755"}:
            fail("unexpected non-regular upstream tree entry: " + relative_path)
        if not is_safe_import_path(relative_path):
            fail("unsafe upstream tree path: " + relative_path)
        entries[relative_path] = hashlib.sha256(
            git_output(upstream, "show", commit + ":" + relative_path)
        ).hexdigest()
    if not entries:
        fail("upstream tree has no imported regular files")
    return entries


def verify_full_upstream(root, metadata, upstream):
    if not (upstream / ".git").exists():
        fail("--upstream must name a local Git checkout")
    expected_entries = upstream_tree_entries(upstream, metadata)
    manifest_entries = read_manifest(root)
    if expected_entries != manifest_entries:
        missing = sorted(set(expected_entries) - set(manifest_entries))
        unexpected = sorted(set(manifest_entries) - set(expected_entries))
        changed = sorted(
            path
            for path in set(expected_entries) & set(manifest_entries)
            if expected_entries[path] != manifest_entries[path]
        )
        details = []
        if missing:
            details.append("missing=" + ", ".join(missing[:3]))
        if unexpected:
            details.append("unexpected=" + ", ".join(unexpected[:3]))
        if changed:
            details.append("changed=" + ", ".join(changed[:3]))
        fail("upstream tree oracle differs from manifest: " + "; ".join(details))


def main(argv):
    parser = argparse.ArgumentParser(
        description=(
            "Offline mode validates the committed import record; --upstream "
            "performs the exact pinned Git-tree comparison."
        )
    )
    parser.add_argument("root", metavar="ROOT")
    parser.add_argument(
        "--upstream",
        type=Path,
        help="local checkout used for an exact pinned Git-tree comparison",
    )
    arguments = parser.parse_args(argv[1:])
    root = Path(arguments.root).resolve()
    if not root.is_dir():
        fail("ROOT is not a directory")
    verify_metadata(root)
    verify_license(root)
    manifest_entries = read_manifest(root)
    overlay_entries = read_overlays(root)
    current_entries = expected_current_entries(manifest_entries, overlay_entries)
    verify_current_tree(root, current_entries)
    if arguments.upstream:
        verify_full_upstream(root, EXPECTED, arguments.upstream.resolve())


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (OSError, ValueError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print("provenance check failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
