#!/usr/bin/env python3
"""Verify a tag-derived unsigned driver build before any signing step."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import re
import runpy
import stat
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RPM = "lto-ltfs-0.1.0-22.el9.x86_64.rpm"
SRPM = "lto-ltfs-0.1.0-22.el9.src.rpm"
SOURCE = "lto-ltfs-0.1.0.tar.gz"
ARTIFACTS = frozenset({
    RPM, SRPM, SOURCE, "SOURCE-MANIFEST.json", "RPM-PAYLOAD-DIGEST",
    "BUILD-INPUTS.json", "SHA256SUMS",
})
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


class PublicDriverBuildError(ValueError):
    """A build input or result is outside the reviewed public contract."""


def _git(repository: Path, *arguments: str) -> str:
    try:
        result = subprocess.run(
            ["/usr/bin/git", "-C", str(repository), *arguments],
            stdin=subprocess.DEVNULL, capture_output=True, text=True,
            check=False, timeout=30,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise PublicDriverBuildError("Git identity check failed") from error
    if result.returncode or len(result.stdout) > 1024 * 1024:
        raise PublicDriverBuildError("Git identity check failed")
    return result.stdout.strip()


def verify_source_ref(repository: Path, tag: str, expected_commit: str) -> int:
    repository = Path(repository)
    if tag != "v0.1.0" or not HEX40.fullmatch(expected_commit):
        raise PublicDriverBuildError("driver tag or commit identity is invalid")
    if (
        _git(repository, "rev-parse", "--show-toplevel") != str(repository.resolve())
        or _git(repository, "rev-parse", "HEAD") != expected_commit
        or _git(repository, "rev-parse", f"refs/tags/{tag}^{{commit}}") != expected_commit
        or _git(repository, "status", "--porcelain", "--untracked-files=all")
    ):
        raise PublicDriverBuildError("driver checkout does not equal clean approved tag")
    epoch = _git(repository, "show", "-s", "--format=%ct", expected_commit)
    if not epoch.isdigit() or int(epoch) <= 0:
        raise PublicDriverBuildError("driver source date is invalid")
    return int(epoch)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _regular(path: Path) -> bool:
    try:
        status = path.lstat()
    except OSError:
        return False
    return stat.S_ISREG(status.st_mode) and status.st_nlink == 1


def _icu_lock(repository: Path) -> dict:
    preparer = runpy.run_path(str(ROOT / "scripts/prepare-icu-build-tools.py"))
    return preparer["load_icu_lock"](Path(repository) / "packaging/rpm/icu-build-tools.json")


def _provider_digests(lock: dict) -> set[str]:
    rows = {row[0]: row for row in lock["payload_members"]}
    return {lock["sha256"], *(rows[path][2] for path in lock["tool_members"])}


def verify_icu_tools(lock: dict, tools: Path) -> None:
    tools = Path(tools)
    if not tools.is_absolute() or tools.is_symlink() or not tools.is_dir() or any(
        parent.is_symlink() for parent in tools.parents
    ):
        raise PublicDriverBuildError("isolated ICU tool directory is unavailable")
    members = list(tools.iterdir())
    expected = {Path(path).name for path in lock["tool_members"]}
    if {path.name for path in members} != expected:
        raise PublicDriverBuildError("isolated ICU tool set differs from approved lock")
    rows = {row[0]: row for row in lock["payload_members"]}
    for path in lock["tool_members"]:
        member = tools / Path(path).name
        if not _regular(member) or stat.S_IMODE(member.lstat().st_mode) != 0o555:
            raise PublicDriverBuildError("isolated ICU executable metadata differs")
        if _sha256(member) != rows[path][2]:
            raise PublicDriverBuildError("isolated ICU executable digest differs")


def _provider_name(name: str) -> bool:
    basename = Path(name).name
    return basename in {"genrb", "pkgdata"} or bool(
        re.fullmatch(r"libicu[^/]*\.so(?:\.[0-9]+)*|icu-[^/]*\.rpm", basename)
    )


def reject_provider_files(root: Path, digests: set[str]) -> None:
    for item in Path(root).rglob("*"):
        if item.is_symlink():
            continue  # The existing RPM payload verifier separately constrains links.
        if item.is_file() and (_provider_name(item.name) or _sha256(item) in digests):
            raise PublicDriverBuildError("distributed payload contains ICU provider bytes")


def reject_provider_archive(archive: Path, digests: set[str]) -> None:
    with tarfile.open(archive, mode="r:gz") as source:
        for member in source:
            if not member.isfile():
                continue
            if _provider_name(member.name):
                raise PublicDriverBuildError("source archive contains an ICU provider file")
            stream = source.extractfile(member)
            if stream is None:
                raise PublicDriverBuildError("source archive member is unreadable")
            digest = hashlib.sha256()
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
            if digest.hexdigest() in digests:
                raise PublicDriverBuildError("source archive contains ICU provider bytes")


def build_inputs_bytes(lock: dict) -> bytes:
    rows = {row[0]: row for row in lock["payload_members"]}
    data = {
        "schema": 1,
        "image": lock["image"],
        "provider": {
            "url": lock["rpm_url"], "nevra": f'{lock["name"]}-{lock["epoch"]}:{lock["version"]}-{lock["release"]}.{lock["arch"]}',
            "sha256": lock["sha256"], "size": lock["size"],
            "key_url": lock["key_url"], "key_fingerprint": lock["key_fingerprint"],
            "key_sha256": lock["key_sha256"],
            "license": lock["license"],
        },
        "build_tools": {Path(path).name: rows[path][2] for path in lock["tool_members"]},
        "runtime_provider_bytes": False,
    }
    return (json.dumps(data, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def verify_output_set(output: Path) -> None:
    output = Path(output)
    if output.is_symlink() or not output.is_dir():
        raise PublicDriverBuildError("unsigned output directory is unsafe")
    members = list(output.iterdir())
    if {member.name for member in members} != ARTIFACTS or any(
        not _regular(member) for member in members
    ):
        raise PublicDriverBuildError("unsigned output differs from exact artifact allowlist")


def verify_hash_manifest(output: Path) -> None:
    verify_output_set(output)
    entries = sorted(ARTIFACTS - {"SHA256SUMS"})
    expected = "".join(f"{_sha256(output / name)}  {name}\n" for name in entries)
    if (output / "SHA256SUMS").read_text(encoding="ascii") != expected:
        raise PublicDriverBuildError("unsigned artifact manifest differs from bytes")


def compare_builds(first: Path, second: Path) -> None:
    verify_output_set(first)
    verify_output_set(second)
    if any(_sha256(Path(first) / name) != _sha256(Path(second) / name)
           for name in ARTIFACTS):
        raise PublicDriverBuildError("independent unsigned builds differ")


def verify_srpm_source_bytes(trusted: Path, extracted: Path) -> None:
    if not _regular(trusted) or not _regular(extracted) or _sha256(trusted) != _sha256(extracted):
        raise PublicDriverBuildError("SRPM Source0 differs from exact tag archive")


def verify_tag_archive(repository: Path, commit: str, archive: Path) -> None:
    if not _regular(archive):
        raise PublicDriverBuildError("tag source archive is missing")
    try:
        producer = subprocess.Popen(
            ["/usr/bin/git", "-C", str(repository), "archive", "--format=tar",
             "--prefix=lto-ltfs-0.1.0/", commit],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        assert producer.stdout is not None
        with gzip.open(archive, "rb") as published:
            while True:
                expected = producer.stdout.read(1024 * 1024)
                actual = published.read(1024 * 1024)
                if expected != actual:
                    raise PublicDriverBuildError("source archive differs from approved tag")
                if not expected:
                    break
        if producer.wait(timeout=60) != 0:
            raise PublicDriverBuildError("approved tag archive cannot be generated")
    except (OSError, EOFError, subprocess.TimeoutExpired) as error:
        raise PublicDriverBuildError("approved tag archive check failed") from error
    finally:
        if "producer" in locals():
            if producer.stdout is not None:
                producer.stdout.close()
            if producer.poll() is None:
                producer.kill()
                producer.wait()


def _conditional_inventory(data: bytes) -> bool:
    try:
        rows = json.loads(data.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        return False
    if not isinstance(rows, list):
        return False
    conditional = [row for row in rows if isinstance(row, dict)
                   and row.get("review_status") == "policy_approved"]
    return len(conditional) == 7 and all(
        row.get("origin_certainty") == "unverified"
        and row.get("approval_basis") == "owner-selected conservative LGPL-2.1-only policy"
        for row in conditional
    )


def verify_installed_notices(source: Path, payload: Path) -> None:
    source = Path(source)
    payload = Path(payload)
    matches = list((payload / "usr/share/doc").glob("lto-ltfs*/license-inventory.json"))
    if len(matches) != 1:
        raise PublicDriverBuildError("installed license inventory is missing or ambiguous")
    pairs = (
        (source / "COPYING.LIB", payload / "usr/share/licenses/lto-ltfs/COPYING.LIB"),
        (source / "LGPL-NOTICE", payload / "usr/share/licenses/lto-ltfs/LGPL-NOTICE"),
        (source / "provenance/license-inventory.json", matches[0]),
    )
    for original, installed in pairs:
        if not _regular(original) or not _regular(installed) or _sha256(original) != _sha256(installed):
            raise PublicDriverBuildError("installed notice differs from approved tag bytes")
    if not _conditional_inventory(pairs[-1][0].read_bytes()):
        raise PublicDriverBuildError("seven conditional origin disclosures are missing")


def verify_preflight(repository: Path, tag: str, commit: str,
                     bundle: Path, lock_path: Path) -> int:
    epoch = verify_source_ref(repository, tag, commit)
    fetcher = runpy.run_path(str(ROOT / "scripts/fetch-public-rpms.py"))
    lock = fetcher["load_lock"](lock_path)
    fetcher["verify_bundle"](lock, bundle)
    return epoch


def authenticate_bundle(bundle: Path, lock_path: Path) -> None:
    fetcher = runpy.run_path(str(ROOT / "scripts/fetch-public-rpms.py"))
    lock = fetcher["load_lock"](lock_path)
    fetcher["verify_bundle"](lock, bundle)
    with tempfile.TemporaryDirectory(prefix="lto-public-keyring-") as raw:
        rpmdb = Path(raw)
        fetcher["_verify_key"](lock, fetcher["IMAGE_GPG_KEY"], rpmdb)
        for row in lock["packages"]:
            fetcher["_verify_rpm"](row, Path(bundle) / row["role"] / row["filename"], rpmdb)


def _verify_rpm_with_existing_script(arguments: list[str]) -> None:
    result = subprocess.run(
        [sys.executable, "-B", str(ROOT / "scripts/verify-rpm.py"), *arguments],
        stdin=subprocess.DEVNULL, capture_output=True, text=True,
        check=False, timeout=240,
    )
    if result.returncode != 0:
        raise PublicDriverBuildError("existing RPM/SRPM verification rejected build")


def verify_output(repository: Path, commit: str, output: Path) -> None:
    verify_hash_manifest(output)
    output = Path(output)
    lock = _icu_lock(repository)
    if (output / "BUILD-INPUTS.json").read_bytes() != build_inputs_bytes(lock):
        raise PublicDriverBuildError("build input manifest differs from approved provider")
    verify_tag_archive(repository, commit, output / SOURCE)
    reject_provider_archive(output / SOURCE, _provider_digests(lock))
    _verify_rpm_with_existing_script([
        "--srpm", str(output / SRPM), "--source-manifest",
        str(output / "SOURCE-MANIFEST.json"),
    ])
    _verify_rpm_with_existing_script([str(output / RPM)])
    inspector = runpy.run_path(str(ROOT / "scripts/verify-rpm.py"))
    records = inspector["read_file_records"](output / RPM)
    with tempfile.TemporaryDirectory(prefix="lto-public-notice-") as raw:
        payload = Path(raw)
        inspector["extract_payload"](output / RPM, payload, records)
        verify_installed_notices(repository, payload)
        reject_provider_files(payload, _provider_digests(lock))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path)
    parser.add_argument("--tag")
    parser.add_argument("--commit")
    parser.add_argument("--bundle", type=Path)
    parser.add_argument("--lock", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--authenticate-bundle", action="store_true")
    parser.add_argument("--authenticate-icu-tools", type=Path)
    parser.add_argument("--write-build-inputs", type=Path)
    parser.add_argument("--compare-first", type=Path)
    parser.add_argument("--compare-second", type=Path)
    args = parser.parse_args()
    if args.compare_first or args.compare_second:
        if (
            not args.compare_first or not args.compare_second
            or any((args.repo, args.tag, args.commit, args.bundle, args.lock,
                    args.output, args.authenticate_bundle, args.authenticate_icu_tools,
                    args.write_build_inputs))
        ):
            parser.error("comparison accepts only two unsigned output directories")
        try:
            verify_hash_manifest(args.compare_first)
            verify_hash_manifest(args.compare_second)
            compare_builds(args.compare_first, args.compare_second)
        except (OSError, UnicodeError, ValueError) as error:
            print(f"public driver comparison rejected: {error}", file=sys.stderr)
            return 1
        return 0
    if not (args.authenticate_icu_tools or args.write_build_inputs) and bool(args.bundle) != bool(args.lock):
        parser.error("--bundle and --lock must be supplied together")
    if args.authenticate_bundle:
        if not args.bundle or args.output or args.repo or args.tag or args.commit or args.authenticate_icu_tools or args.write_build_inputs:
            parser.error("bundle authentication accepts only --bundle and --lock")
        try:
            authenticate_bundle(args.bundle, args.lock)
        except (OSError, UnicodeError, ValueError, subprocess.TimeoutExpired) as error:
            print(f"public driver build rejected: {error}", file=sys.stderr)
            return 1
        return 0
    if args.authenticate_icu_tools or args.write_build_inputs:
        if not args.lock or args.bundle or args.output or args.repo or args.tag or args.commit:
            parser.error("ICU tool admission accepts only --lock and one ICU action")
        if bool(args.authenticate_icu_tools) == bool(args.write_build_inputs):
            parser.error("select exactly one ICU action")
        try:
            lock = runpy.run_path(str(ROOT / "scripts/prepare-icu-build-tools.py"))["load_icu_lock"](args.lock)
            if args.authenticate_icu_tools:
                verify_icu_tools(lock, args.authenticate_icu_tools)
            else:
                target = args.write_build_inputs
                if target.exists() or target.is_symlink() or not target.parent.is_dir():
                    raise PublicDriverBuildError("build input output must be a new file")
                target.write_bytes(build_inputs_bytes(lock))
        except (OSError, UnicodeError, ValueError) as error:
            print(f"public driver build rejected: {error}", file=sys.stderr)
            return 1
        return 0
    if not args.repo or not args.tag or not args.commit or bool(args.output) == bool(args.bundle):
        parser.error("select exactly one of --bundle/--lock or --output")
    try:
        epoch = verify_source_ref(args.repo, args.tag, args.commit)
        if args.bundle:
            verify_preflight(args.repo, args.tag, args.commit, args.bundle, args.lock)
            print(epoch)
        else:
            verify_output(args.repo, args.commit, args.output)
    except (OSError, UnicodeError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"public driver build rejected: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
