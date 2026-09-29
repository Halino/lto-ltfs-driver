"""Exact authenticated, build-only ICU provider closure."""

from __future__ import annotations

import copy
import hashlib
import os
import runpy
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts/prepare-icu-build-tools.py"
LOCK = ROOT / "packaging/rpm/icu-build-tools.json"


class IcuBuildToolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = runpy.run_path(str(TOOL))

    def test_icu_lock_rejects_unlisted_origin_and_nevra(self) -> None:
        load = self.tool["load_icu_lock"]
        error = self.tool["IcuToolError"]
        lock = load(LOCK)
        self.assertEqual("icu", lock["name"])
        self.assertEqual("67.1", lock["version"])
        for key, value in (
            ("rpm_url", lock["rpm_url"].replace("https://", "http://")),
            ("rpm_url", lock["rpm_url"].replace("mirror.stream.centos.org", "mirror.example.test")),
            ("version", "67.2"),
            ("release", "10.el9_6"),
            ("arch", "aarch64"),
            ("key_fingerprint", "A" * 40),
        ):
            with self.subTest(key=key, value=value), self.assertRaises(error):
                self.tool["validate_icu_lock"](copy.deepcopy(lock) | {key: value})
        with tempfile.TemporaryDirectory() as raw:
            duplicate = Path(raw) / "duplicate.json"
            duplicate.write_text(LOCK.read_text().replace('"schema": 1,', '"schema": 1, "schema": 1,'))
            with self.assertRaises(error):
                load(duplicate)

    def test_icu_rpm_requires_official_key_and_signature(self) -> None:
        lock = self.tool["load_icu_lock"](LOCK)
        verify = self.tool["_verify_rpm_identity"]
        error = self.tool["IcuToolError"]
        header = (
            "icu|0|67.1|10.el9|x86_64|" + lock["license"]
            + "|RSA/SHA256, Key ID 05b555b38483c65d|"
            + "RSA/SHA256, Key ID 05b555b38483c65d\n"
        )
        signature = (
            "Header V4 RSA/SHA256 Signature, key ID 8483c65d: OK\n"
            "Payload SHA256 digest: OK\n"
            "V4 RSA/SHA256 Signature, key ID 8483c65d: OK\n"
        )
        inventory = "".join("|".join(row) + "\n" for row in lock["payload_members"])

        def runner(argv):
            if argv[0].endswith("rpmkeys"):
                return signature
            if "FILEMODES:octal" in " ".join(argv):
                return inventory
            return header

        verify(lock, Path("/tmp/icu.rpm"), Path("/tmp/rpmdb"), runner=runner)
        for bad in (
            lambda argv: signature.replace("8483c65d", "11111111") if argv[0].endswith("rpmkeys") else runner(argv),
            lambda argv: signature.replace("Payload SHA256 digest: OK", "Payload SHA256 digest: BAD") if argv[0].endswith("rpmkeys") else runner(argv),
            lambda argv: header.replace("x86_64", "aarch64") if "FILEMODES:octal" not in " ".join(argv) and not argv[0].endswith("rpmkeys") else runner(argv),
            lambda argv: inventory + "/usr/bin/unlisted|100755|" + "a" * 64 + "|\n" if "FILEMODES:octal" in " ".join(argv) else runner(argv),
        ):
            with self.subTest(bad=bad), self.assertRaises(error):
                verify(lock, Path("/tmp/icu.rpm"), Path("/tmp/rpmdb"), runner=bad)

    def test_download_rejects_redirect_even_with_matching_bytes(self) -> None:
        lock = self.tool["load_icu_lock"](LOCK)
        error = self.tool["IcuToolError"]

        class Response:
            status = 200

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return None

            def geturl(self):
                return "https://mirror.example.test/icu-67.1-10.el9.x86_64.rpm"

            def read(self, _size):
                return b""

        class Opener:
            def open(self, *_args, **_kwargs):
                return Response()

        with tempfile.TemporaryDirectory() as raw:
            target = Path(raw) / "icu.rpm"
            with self.assertRaises(error):
                self.tool["_download_verified"](
                    lock["rpm_url"], target, lock["size"], lock["sha256"], Opener()
                )
            self.assertFalse(target.exists())

    def test_only_reviewed_regular_executables_are_extracted(self) -> None:
        lock = self.tool["load_icu_lock"](LOCK)
        select = self.tool["_copy_reviewed_tools"]
        error = self.tool["IcuToolError"]
        contents = {"genrb": b"fixture-genrb", "pkgdata": b"fixture-pkgdata"}
        fixture = copy.deepcopy(lock)
        for row in fixture["payload_members"]:
            if Path(row[0]).name in contents and row[0] in fixture["tool_members"]:
                row[2] = hashlib.sha256(contents[Path(row[0]).name]).hexdigest()
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            extracted = root / "extracted"
            tools = root / "tools"
            source = extracted / "usr/bin"
            source.mkdir(parents=True)
            for name, value in contents.items():
                (source / name).write_bytes(value)
                (source / name).chmod(0o755)
            select(fixture, extracted, tools)
            self.assertEqual(set(contents), {p.name for p in tools.iterdir()})
            (source / "extra-helper").write_bytes(b"extra")
            with self.assertRaises(error):
                select(fixture, extracted, root / "extra-output")
            (source / "extra-helper").unlink()
            (source / "genrb").unlink()
            (source / "genrb").symlink_to("pkgdata")
            with self.assertRaises(error):
                select(fixture, extracted, root / "symlink-output")
            (source / "genrb").unlink()
            (source / "genrb").write_bytes(contents["genrb"])
            (source / "genrb").chmod(0o4755)
            with self.assertRaises(error):
                select(fixture, extracted, root / "setuid-output")
            (source / "genrb").chmod(0o755)
            (source / "pkgdata").write_bytes(b"changed")
            with self.assertRaises(error):
                select(fixture, extracted, root / "digest-output")


if __name__ == "__main__":
    unittest.main()
