#!/usr/bin/env python3
"""Authenticate one CentOS ICU RPM as an isolated build-tool provider.

The package is never installed. Only separately reviewed tools may later be
extracted; UBI remains the library and header authority.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib import error as urlerror
from urllib import request as urlrequest


IMAGE = (
    "registry.access.redhat.com/ubi9/ubi@sha256:"
    "5426a8f45e80a07168a30ea24d84f266094b3756624a5508cc53927e6ee39e09"
)
REPOSITORY = "https://mirror.stream.centos.org/9-stream/AppStream/x86_64/os/Packages/"
RPM_URL = REPOSITORY + "icu-67.1-10.el9.x86_64.rpm"
KEY_URL = "https://www.centos.org/keys/RPM-GPG-KEY-CentOS-Official-SHA256"
RPM_SHA256 = "ec6752eaaddc260e294936ed55fc6a19f086b1193c51944962505346ad86d8f7"
KEY_SHA256 = "5af55449d6c9bc594e2e2fb7222374cb25a8ad2d8ea6ce3de894a3201944daa2"
KEY_FINGERPRINT = "99DB70FAE1D7CE227FB6488205B555B38483C65D"
LICENSE = "Unicode-DFS-2016 AND BSD-2-Clause AND BSD-3-Clause AND LicenseRef-Fedora-Public-Domain"
SHA = re.compile(r"[0-9a-f]{64}\Z")


class IcuToolError(RuntimeError):
    """The build-only provider escaped its reviewed identity or payload."""


class _NoRedirect(urlrequest.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        return None


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _duplicate_free(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise IcuToolError("duplicate ICU lock JSON key")
        value[key] = item
    return value


def validate_icu_lock(lock: object) -> None:
    if not isinstance(lock, dict) or set(lock) != {
        "schema", "image", "repository_url", "rpm_url", "filename", "name",
        "epoch", "version", "release", "arch", "size", "sha256", "key_url",
        "key_sha256", "key_fingerprint", "license", "payload_members", "tool_members",
    }:
        raise IcuToolError("ICU lock keys differ from the reviewed schema")
    exact = {
        "schema": 1, "image": IMAGE, "repository_url": REPOSITORY,
        "rpm_url": RPM_URL, "filename": "icu-67.1-10.el9.x86_64.rpm",
        "name": "icu", "epoch": "0", "version": "67.1", "release": "10.el9",
        "arch": "x86_64", "size": 239643, "sha256": RPM_SHA256,
        "key_url": KEY_URL, "key_sha256": KEY_SHA256,
        "key_fingerprint": KEY_FINGERPRINT, "license": LICENSE,
    }
    if any(type(lock[key]) is not type(value) or lock[key] != value
           for key, value in exact.items()):
        raise IcuToolError("ICU origin, key, NEVRA, license or bytes differ")
    members = lock["payload_members"]
    if not isinstance(members, list) or not members:
        raise IcuToolError("ICU payload inventory is absent")
    paths = []
    for member in members:
        if not isinstance(member, list) or len(member) != 4 or any(
            type(field) is not str for field in member
        ):
            raise IcuToolError("ICU payload inventory row is malformed")
        path, mode, digest, target = member
        if (
            not path.startswith("/") or path == "/" or ".." in Path(path).parts
            or "//" in path or "\\" in path or "|" in path
            or not re.fullmatch(r"[0-7]{5,6}", mode)
        ):
            raise IcuToolError("ICU payload path or mode is unsafe")
        kind = stat.S_IFMT(int(mode, 8))
        if kind == stat.S_IFREG:
            if not SHA.fullmatch(digest) or target:
                raise IcuToolError("ICU regular-file digest differs")
        elif kind == stat.S_IFDIR:
            if digest or target:
                raise IcuToolError("ICU directory inventory differs")
        elif kind == stat.S_IFLNK:
            if digest or not target or "|" in target or target.startswith("/"):
                raise IcuToolError("ICU symlink inventory differs")
        else:
            raise IcuToolError("ICU payload contains an unsupported file type")
        paths.append(path)
    if paths != sorted(set(paths)):
        raise IcuToolError("ICU payload inventory is not unique and sorted")
    tools = lock["tool_members"]
    if (
        not isinstance(tools, list) or tools != ["/usr/bin/genrb", "/usr/bin/pkgdata"]
        or any(path not in paths for path in tools)
        or any(stat.S_IFMT(int(members[paths.index(path)][1], 8)) != stat.S_IFREG
               or not int(members[paths.index(path)][1], 8) & 0o111 for path in tools)
    ):
        raise IcuToolError("ICU build-tool subset differs from reviewed executables")


def load_icu_lock(path: Path) -> dict:
    path = Path(path)
    try:
        status = path.lstat()
        if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
            raise IcuToolError("ICU lock is not an ordinary single-link file")
        contents = path.read_bytes()
        lock = json.loads(contents.decode("utf-8"), object_pairs_hook=_duplicate_free)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise IcuToolError("ICU lock cannot be read") from error
    validate_icu_lock(lock)
    if contents != (json.dumps(lock, sort_keys=True, indent=2) + "\n").encode():
        raise IcuToolError("ICU lock is not canonical JSON")
    return lock


def _download_verified(url: str, target: Path, size: int | None,
                       digest: str, opener=None) -> None:
    if target.exists() or target.is_symlink() or not SHA.fullmatch(digest):
        raise IcuToolError("ICU download destination or digest is invalid")
    if opener is None:
        opener = urlrequest.build_opener(_NoRedirect())
    maximum = size if size is not None else 1024 * 1024
    seen = hashlib.sha256()
    received = 0
    created = False
    complete = False
    try:
        request = urlrequest.Request(url, headers={"User-Agent": "lto-icu-build-lock/1"})
        with opener.open(request, timeout=30) as response:
            if response.status != 200 or response.geturl() != url:
                raise IcuToolError("ICU provider URL redirected or changed")
            with target.open("xb") as output:
                created = True
                while block := response.read(1024 * 1024):
                    received += len(block)
                    if received > maximum:
                        raise IcuToolError("ICU provider exceeds locked size")
                    seen.update(block)
                    output.write(block)
                output.flush()
                os.fsync(output.fileno())
        if (size is not None and received != size) or seen.hexdigest() != digest:
            raise IcuToolError("ICU provider bytes differ from lock")
        complete = True
    except (OSError, urlerror.URLError) as error:
        raise IcuToolError("ICU provider download failed") from error
    finally:
        if created and not complete:
            target.unlink(missing_ok=True)


def _run_checked(argv: list[str]) -> str:
    try:
        result = subprocess.run(argv, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise IcuToolError("ICU verification command could not run") from error
    if result.returncode or len(result.stdout) > 1024 * 1024 or len(result.stderr) > 1024 * 1024:
        raise IcuToolError("ICU verification command failed")
    return result.stdout


def _verify_key(lock: dict, key: Path, rpmdb: Path) -> None:
    if key.is_symlink() or not key.is_file() or _sha256(key) != lock["key_sha256"]:
        raise IcuToolError("official CentOS key differs from lock")
    listing = _run_checked(["/usr/bin/gpg", "--batch", "--with-colons", "--show-keys", str(key)])
    fingerprints = [line.split(":")[9] for line in listing.splitlines() if line.startswith("fpr:")]
    if fingerprints != [lock["key_fingerprint"]]:
        raise IcuToolError("CentOS key fingerprint differs from lock")
    _run_checked(["/usr/bin/rpm", "--dbpath", str(rpmdb), "--initdb"])
    _run_checked(["/usr/bin/rpmkeys", "--dbpath", str(rpmdb), "--import", str(key)])


def _verify_rpm_identity(lock: dict, rpm: Path, rpmdb: Path, *, runner=None) -> None:
    if runner is None:
        runner = _run_checked
    query = runner([
        "/usr/bin/rpm", "-qp", "--qf",
        "%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}|"
        "%{LICENSE}|%{SIGPGP:pgpsig}|%{RSAHEADER:pgpsig}\\n", str(rpm),
    ])
    fields = query.removesuffix("\n").split("|")
    if len(fields) != 8 or query.count("\n") != 1 or fields[:6] != [
        lock[key] for key in ("name", "epoch", "version", "release", "arch", "license")
    ]:
        raise IcuToolError("ICU RPM header identity differs from lock")
    if any(lock["key_fingerprint"][-16:].lower() not in field.lower()
           for field in fields[6:]):
        raise IcuToolError("ICU RPM header signer differs from official key")
    signature = runner(["/usr/bin/rpmkeys", "--dbpath", str(rpmdb), "-Kv", str(rpm)])
    key_id = lock["key_fingerprint"][-8:].lower()
    if (
        re.search(rf"(?m)^\s*Header V4 RSA/SHA256 Signature, key ID {key_id}: OK$", signature) is None
        or re.search(rf"(?m)^\s*V4 RSA/SHA256 Signature, key ID {key_id}: OK$", signature) is None
        or "Payload SHA256 digest: OK" not in signature
        or "BAD" in signature or "NOKEY" in signature
    ):
        raise IcuToolError("ICU RPM signature or payload is not verified")
    inventory = runner([
        "/usr/bin/rpm", "-qp", "--qf",
        "[%{FILENAMES}|%{FILEMODES:octal}|%{FILEDIGESTS}|%{FILELINKTOS}\\n]",
        str(rpm),
    ])
    rows = [line.split("|") for line in inventory.splitlines()]
    if rows != lock["payload_members"]:
        raise IcuToolError("ICU signed payload inventory differs from lock")


def fetch_icu_rpm(lock: dict, output: Path) -> Path:
    validate_icu_lock(lock)
    output = Path(output)
    if (
        not output.is_absolute() or output.exists() or output.is_symlink()
        or not output.parent.is_dir() or any(parent.is_symlink() for parent in output.parents)
    ):
        raise IcuToolError("ICU fetch needs a new absolute output directory")
    with tempfile.TemporaryDirectory(prefix=".lto-icu-", dir=output.parent) as raw:
        stage = Path(raw)
        rpm = stage / lock["filename"]
        key = stage / "RPM-GPG-KEY-CentOS-Official-SHA256"
        _download_verified(lock["rpm_url"], rpm, lock["size"], lock["sha256"])
        _download_verified(lock["key_url"], key, None, lock["key_sha256"])
        with tempfile.TemporaryDirectory(prefix=".lto-icu-rpmdb-", dir=output.parent) as db:
            _verify_key(lock, key, Path(db))
            _verify_rpm_identity(lock, rpm, Path(db))
        stage.chmod(0o755)
        os.replace(stage, output)
    return output / lock["filename"]


def _cpio_members(rpm: Path, directory: Path, selected: list[str] | None) -> str:
    command = ["/usr/bin/cpio", "-it", "--quiet"] if selected is None else [
        "/usr/bin/cpio", "-idm", "--no-absolute-filenames", "--quiet", *selected,
    ]
    try:
        producer = subprocess.Popen(
            ["/usr/bin/rpm2cpio", str(rpm)], stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, stdin=subprocess.DEVNULL,
        )
        assert producer.stdout is not None
        consumer = subprocess.run(
            command, cwd=directory, stdin=producer.stdout, capture_output=True,
            text=True, timeout=60,
        )
        producer.stdout.close()
        producer_stderr = producer.communicate(timeout=60)[1]
    except (OSError, subprocess.TimeoutExpired) as error:
        raise IcuToolError("ICU archive inventory or extraction failed") from error
    if (
        producer.returncode or consumer.returncode
        or len(consumer.stdout) > 1024 * 1024 or len(consumer.stderr) > 1024 * 1024
        or len(producer_stderr) > 1024 * 1024
    ):
        raise IcuToolError("ICU archive inventory or extraction failed")
    return consumer.stdout


def _copy_reviewed_tools(lock: dict, extracted: Path, tools: Path) -> None:
    if extracted.is_symlink() or not extracted.is_dir() or tools.exists() or tools.is_symlink():
        raise IcuToolError("ICU tool staging path is unsafe")
    expected_files = {path.removeprefix("/") for path in lock["tool_members"]}
    expected_dirs = {"usr", "usr/bin"}
    actual = {path.relative_to(extracted).as_posix(): path for path in extracted.rglob("*")}
    if set(actual) != expected_files | expected_dirs:
        raise IcuToolError("ICU extraction contains an unreviewed member")
    for directory in expected_dirs:
        if not stat.S_ISDIR(actual[directory].lstat().st_mode) or actual[directory].is_symlink():
            raise IcuToolError("ICU extracted directory is unsafe")
    rows = {row[0]: row for row in lock["payload_members"]}
    for member in lock["tool_members"]:
        source = actual[member.removeprefix("/")]
        status = source.lstat()
        mode = int(rows[member][1], 8)
        if (
            not stat.S_ISREG(status.st_mode) or status.st_nlink != 1
            or stat.S_IMODE(status.st_mode) != stat.S_IMODE(mode)
            or status.st_mode & 0o6000 or mode & 0o6000
            or _sha256(source) != rows[member][2]
        ):
            raise IcuToolError("ICU extracted executable differs from signed inventory")
    tools.mkdir(mode=0o755)
    for member in lock["tool_members"]:
        source = actual[member.removeprefix("/")]
        target = tools / Path(member).name
        shutil.copyfile(source, target, follow_symlinks=False)
        if _sha256(target) != rows[member][2]:
            raise IcuToolError("ICU copied executable differs from lock")
        target.chmod(0o555)
    tools.chmod(0o555)


def prepare_icu_tools(lock_path: Path, output: Path) -> Path:
    lock = load_icu_lock(lock_path)
    output = Path(output)
    if (
        not output.is_absolute() or output.exists() or output.is_symlink()
        or not output.parent.is_dir() or any(parent.is_symlink() for parent in output.parents)
    ):
        raise IcuToolError("ICU tools need a new absolute output directory")
    with tempfile.TemporaryDirectory(prefix=".lto-icu-provider-", dir=output.parent) as raw:
        scratch = Path(raw)
        rpm = fetch_icu_rpm(lock, scratch / "fetched")
        inventory = _cpio_members(rpm, scratch, None)
        expected = ["." + row[0] for row in lock["payload_members"]]
        if inventory.splitlines() != expected:
            raise IcuToolError("ICU archive member closure differs from signed header")
        extracted = scratch / "extracted"
        extracted.mkdir()
        _cpio_members(rpm, extracted, ["." + member for member in lock["tool_members"]])
        with tempfile.TemporaryDirectory(prefix=".lto-icu-tools-", dir=output.parent) as stage_raw:
            stage = Path(stage_raw)
            _copy_reviewed_tools(lock, extracted, stage / "tools")
            stage.chmod(0o755)
            os.replace(stage, output)
    return output / "tools"


def validate_ubi_loader_report(report: str) -> set[str]:
    """Reject unresolved symbols or any ICU library outside the UBI loader path."""
    if type(report) is not str or "not found" in report or "undefined symbol" in report:
        raise IcuToolError("ICU tool cannot load against UBI libraries")
    resolved: set[str] = set()
    names: set[str] = set()
    for line in report.splitlines():
        if "libicu" not in line:
            continue
        match = re.fullmatch(
            r"\s*(libicu[A-Za-z0-9_]*\.so\.67) => (/\S+) \(0x[0-9a-fA-F]+\)\s*",
            line,
        )
        if match is None or match.group(2) not in {
            f"/lib64/{match.group(1)}", f"/usr/lib64/{match.group(1)}",
        }:
            raise IcuToolError("ICU tool resolved an unreviewed library")
        names.add(match.group(1))
        resolved.add(match.group(2))
    if "libicuuc.so.67" not in names:
        raise IcuToolError("ICU tool did not resolve UBI libicuuc")
    return resolved


def validate_pkgdata_config(contents: str) -> None:
    values = {}
    for line in contents.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            if key in values:
                raise IcuToolError("duplicate UBI pkgdata helper setting")
            values[key] = value
    if (
        values.get("GENCCODE_ASSEMBLY_TYPE") != "-a gcc"
        or not values.get("COMPILE", "").startswith("gcc ")
        or values.get("AR") != "ar" or values.get("RANLIB") != "ranlib"
    ):
        raise IcuToolError("UBI pkgdata compiler/archive helpers differ")


def verify_catalog_pair(first: Path, second: Path) -> None:
    for path in (first, second):
        status = Path(path).lstat()
        if (
            not stat.S_ISREG(status.st_mode) or status.st_nlink != 1
            or status.st_size <= 4 or not Path(path).read_bytes().startswith(b"\x7fELF")
        ):
            raise IcuToolError("generated ICU catalog is not a nonempty ELF object")
    if _sha256(first) != _sha256(second):
        raise IcuToolError("ICU catalog bytes are not repeatable")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    try:
        rpm = (prepare_icu_tools(args.lock, args.output) if args.prepare
               else fetch_icu_rpm(load_icu_lock(args.lock), args.output))
    except (IcuToolError, OSError, ValueError) as error:
        print(f"ICU build-tool provider refused: {error}", file=sys.stderr)
        return 1
    print(rpm)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
