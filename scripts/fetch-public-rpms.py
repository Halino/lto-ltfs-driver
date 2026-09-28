#!/usr/bin/env python3
"""Fetch an exact public RPM closure; never use host DNF caches as authority."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib import error as urlerror
from urllib import request as urlrequest
from urllib.parse import urlsplit


HEX64 = re.compile(r"[0-9a-f]{64}\Z")
FINGERPRINT = re.compile(r"[0-9A-F]{40}\Z")
ATOM = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+~-]*\Z")
PINNED_IMAGE = (
    "registry.access.redhat.com/ubi9/ubi@sha256:"
    "5426a8f45e80a07168a30ea24d84f266094b3756624a5508cc53927e6ee39e09"
)
IMAGE_GPG_KEY = Path("/etc/pki/rpm-gpg/RPM-GPG-KEY-redhat-release")


class DependencyError(RuntimeError):
    """A package or repository escaped the exact reviewed public closure."""


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _safe_https(url: object) -> bool:
    if not isinstance(url, str) or not url:
        return False
    try:
        parsed = urlsplit(url)
        port = parsed.port
    except ValueError:
        return False
    return (
        parsed.scheme == "https"
        and bool(parsed.hostname)
        and parsed.username is None
        and parsed.password is None
        and port in (None, 443)
        and not parsed.query
        and not parsed.fragment
        and parsed.path.startswith("/")
        and ".." not in parsed.path.split("/")
        and "\\" not in parsed.path
    )


def validate_lock(lock: object) -> None:
    if not isinstance(lock, dict) or set(lock) != {
        "schema", "image", "gpg_key_sha256", "gpg_fingerprints",
        "repositories", "packages",
    }:
        raise DependencyError("public dependency lock has invalid keys")
    if (
        type(lock["schema"]) is not int or lock["schema"] != 1
        or not isinstance(lock["image"], str)
        or lock["image"] != PINNED_IMAGE
        or not isinstance(lock["gpg_key_sha256"], str)
        or not HEX64.fullmatch(lock["gpg_key_sha256"])
    ):
        raise DependencyError("public dependency lock has invalid authority")
    fingerprints = lock["gpg_fingerprints"]
    if (
        not isinstance(fingerprints, list) or not fingerprints
        or any(not isinstance(item, str) or not FINGERPRINT.fullmatch(item)
               for item in fingerprints)
        or fingerprints != sorted(set(fingerprints))
    ):
        raise DependencyError("public RPM signing key identity is invalid")
    repositories = lock["repositories"]
    if not isinstance(repositories, list) or not repositories:
        raise DependencyError("no public repositories are locked")
    repository_map = {}
    for repository in repositories:
        if (
            not isinstance(repository, dict)
            or set(repository) != {"id", "base_url"}
            or not isinstance(repository["id"], str)
            or not ATOM.fullmatch(repository["id"])
            or repository["id"] in repository_map
            or not _safe_https(repository["base_url"])
            or not repository["base_url"].endswith("/")
        ):
            raise DependencyError("public repository authority is invalid")
        repository_map[repository["id"]] = repository["base_url"]
    if list(repository_map) != sorted(repository_map):
        raise DependencyError("public repositories are not sorted")
    packages = lock["packages"]
    if not isinstance(packages, list) or not packages:
        raise DependencyError("public RPM closure is empty")
    seen = set()
    roles = set()
    for package in packages:
        if not isinstance(package, dict) or set(package) != {
            "role", "repository", "url", "filename", "name", "epoch",
            "version", "release", "arch", "sha256", "size", "signer_fingerprint",
        }:
            raise DependencyError("public RPM row has invalid keys")
        role = package["role"]
        name = package["name"]
        filename = package["filename"]
        arch = package["arch"]
        if (
            not isinstance(role, str)
            or role not in {"build", "runtime"}
            or not isinstance(package["repository"], str)
            or package["repository"] not in repository_map
            or any(not isinstance(package[key], str) or not ATOM.fullmatch(package[key])
                   for key in ("name", "version", "release", "arch"))
            or not isinstance(arch, str)
            or arch not in {"x86_64", "noarch"}
            or not isinstance(package["epoch"], str)
            or not package["epoch"].isdigit()
            or not isinstance(filename, str)
            or filename != f"{name}-{package['version']}-{package['release']}.{arch}.rpm"
            or (role, filename) in seen
            or not isinstance(package["sha256"], str)
            or not HEX64.fullmatch(package["sha256"])
            or type(package["size"]) is not int
            or not 0 < package["size"] <= 512 * 1024 * 1024
            or package["signer_fingerprint"] not in fingerprints
            or not _safe_https(package["url"])
            or not package["url"].startswith(repository_map[package["repository"]])
            or not package["url"].endswith("/" + filename)
        ):
            raise DependencyError("public RPM identity or URL is invalid")
        seen.add((role, filename))
        roles.add(role)
    if roles != {"build", "runtime"}:
        raise DependencyError("build/runtime RPM closure is incomplete")
    if [(row["role"], row["filename"]) for row in packages] != sorted(seen):
        raise DependencyError("public RPM closure is not sorted")


def verify_bundle(lock: dict, bundle: Path) -> None:
    validate_lock(lock)
    bundle = Path(bundle)
    if bundle.is_symlink() or not bundle.is_dir():
        raise DependencyError("RPM bundle directory is unsafe")
    expected = {(row["role"], row["filename"]): row for row in lock["packages"]}
    actual = {}
    if {entry.name for entry in bundle.iterdir()} != {"build", "runtime"}:
        raise DependencyError("RPM bundle contains unexpected entries")
    for role in ("build", "runtime"):
        directory = bundle / role
        if directory.is_symlink() or not directory.is_dir():
            raise DependencyError("RPM bundle role directory is unsafe")
        for path in directory.iterdir():
            status = path.lstat()
            if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
                raise DependencyError("RPM bundle contains an unsafe member")
            actual[(role, path.name)] = (status.st_size, _sha256(path))
    if set(actual) != set(expected):
        raise DependencyError("RPM bundle differs from the locked closure")
    for key, (size, digest) in actual.items():
        row = expected[key]
        if size != row["size"] or digest != row["sha256"]:
            raise DependencyError("RPM bundle member bytes differ from lock")


class _NoRedirect(urlrequest.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        return None


def _download_verified(row: dict, target: Path, opener=None) -> None:
    """Stream one locked URL; a redirect or changed byte is not a substitute."""
    if target.exists() or target.is_symlink():
        raise DependencyError("public RPM output already exists")
    if opener is None:
        opener = urlrequest.build_opener(_NoRedirect())
    digest = hashlib.sha256()
    size = 0
    created = False
    complete = False
    try:
        request = urlrequest.Request(
            row["url"], headers={"User-Agent": "lto-ltfs-public-lock/1"}
        )
        with opener.open(request, timeout=30) as response:
            if response.status != 200 or response.geturl() != row["url"]:
                raise DependencyError("public RPM URL redirected or changed")
            with target.open("xb") as output:
                created = True
                while block := response.read(1024 * 1024):
                    size += len(block)
                    if size > row["size"]:
                        raise DependencyError("public RPM exceeds locked byte size")
                    digest.update(block)
                    output.write(block)
        if size != row["size"] or digest.hexdigest() != row["sha256"]:
            raise DependencyError("downloaded RPM differs from locked bytes")
        complete = True
    except (OSError, urlerror.URLError) as error:
        raise DependencyError("public RPM download failed") from error
    finally:
        if created and not complete:
            target.unlink(missing_ok=True)


def _run_checked(arguments: list[str]) -> str:
    try:
        result = subprocess.run(
            arguments, stdin=subprocess.DEVNULL, capture_output=True,
            text=True, check=False, timeout=60,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise DependencyError("public RPM verification tool failed") from error
    if (
        result.returncode != 0 or len(result.stdout) > 64 * 1024
        or len(result.stderr) > 64 * 1024
    ):
        raise DependencyError("public RPM verification tool failed")
    return result.stdout


def _verify_rpm(row: dict, rpm: Path, rpmdb: Path, *, runner=None) -> None:
    if runner is None:
        runner = _run_checked
    query = runner([
        "/usr/bin/rpm", "-qp", "--qf",
        "%{NAME}|%{EPOCHNUM}|%{VERSION}|%{RELEASE}|%{ARCH}|"
        "%{SIGPGP:pgpsig}|%{RSAHEADER:pgpsig}\\n", str(rpm),
    ])
    fields = query.removesuffix("\n").split("|")
    if len(fields) != 7 or query.count("\n") != 1:
        raise DependencyError("public RPM header query is malformed")
    if fields[:5] != [row[key] for key in ("name", "epoch", "version", "release", "arch")]:
        raise DependencyError("public RPM NEVRA differs from lock")
    key_ids = set(re.findall(r"Key ID ([0-9a-f]{16})", "|".join(fields[5:])))
    if key_ids != {row["signer_fingerprint"][-16:].lower()}:
        raise DependencyError("public RPM signing identity differs from lock")
    signature = runner(["/usr/bin/rpmkeys", "--dbpath", str(rpmdb), "-Kv", str(rpm)])
    key_id = row["signer_fingerprint"][-8:].lower()
    if (
        re.search(r"Signature, key ID " + key_id + r": OK(?:\n|$)", signature)
        is None
        or "NOKEY" in signature or "BAD" in signature
    ):
        raise DependencyError("public RPM signature is not verified")


def _duplicate_free(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise DependencyError("duplicate public lock JSON key")
        value[key] = item
    return value


def load_lock(path: Path) -> dict:
    path = Path(path)
    try:
        status = path.lstat()
        if not stat.S_ISREG(status.st_mode) or status.st_nlink != 1:
            raise DependencyError("public dependency lock file is unsafe")
        data = path.read_bytes()
        lock = json.loads(data.decode("utf-8"), object_pairs_hook=_duplicate_free)
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise DependencyError("public dependency lock cannot be read") from error
    validate_lock(lock)
    canonical = (json.dumps(lock, sort_keys=True, indent=2) + "\n").encode("utf-8")
    if data != canonical:
        raise DependencyError("public dependency lock is not canonical")
    return lock


def _verify_key(lock: dict, key: Path, rpmdb: Path) -> None:
    try:
        status = key.lstat()
    except OSError as error:
        raise DependencyError("pinned image RPM key is unavailable") from error
    if (
        not stat.S_ISREG(status.st_mode) or status.st_nlink != 1
        or _sha256(key) != lock["gpg_key_sha256"]
    ):
        raise DependencyError("pinned image RPM key differs from lock")
    listed = _run_checked(["/usr/bin/gpg", "--batch", "--with-colons", "--show-keys", str(key)])
    fingerprints = sorted(
        line.split(":")[9] for line in listed.splitlines() if line.startswith("fpr:")
    )
    if fingerprints != lock["gpg_fingerprints"]:
        raise DependencyError("pinned image RPM key fingerprints differ from lock")
    _run_checked(["/usr/bin/rpm", "--dbpath", str(rpmdb), "--initdb"])
    _run_checked(["/usr/bin/rpm", "--dbpath", str(rpmdb), "--import", str(key)])


def fetch_public_rpms(lock_path: Path, output: Path) -> None:
    lock = load_lock(lock_path)
    output = Path(output)
    if (
        not output.is_absolute() or output.exists() or output.is_symlink()
        or not output.parent.is_dir()
        or any(parent.is_symlink() for parent in output.parents)
    ):
        raise DependencyError("public RPM output must be a new absolute directory")
    with tempfile.TemporaryDirectory(prefix=".lto-public-rpms-", dir=output.parent) as raw:
        root = Path(raw)
        bundle = root / "rpm-bundle"
        for role in ("build", "runtime"):
            (bundle / role).mkdir(parents=True)
        with tempfile.TemporaryDirectory(prefix=".lto-rpmdb-", dir=output.parent) as db_raw:
            rpmdb = Path(db_raw)
            _verify_key(lock, IMAGE_GPG_KEY, rpmdb)
            for row in lock["packages"]:
                package = bundle / row["role"] / row["filename"]
                _download_verified(row, package)
                _verify_rpm(row, package, rpmdb)
        verify_bundle(lock, bundle)
        manifest = root / "RPM-BUNDLE.sha256"
        with manifest.open("x", encoding="ascii") as stream:
            for row in lock["packages"]:
                stream.write(f"{row['sha256']}  {row['role']}/{row['filename']}\n")
            stream.flush()
            os.fsync(stream.fileno())
        root.chmod(0o755)
        os.replace(root, output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        fetch_public_rpms(args.lock, args.output)
    except DependencyError as error:
        print(f"public dependency fetch rejected: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
