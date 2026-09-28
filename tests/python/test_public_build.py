"""Fail-closed checks for the tag-derived, unsigned public driver build."""

from __future__ import annotations

import json
import gzip
import runpy
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "scripts/verify-public-build.py"
RPM = "lto-ltfs-0.1.0-22.el9.x86_64.rpm"
SRPM = "lto-ltfs-0.1.0-22.el9.src.rpm"
ARTIFACTS = {
    RPM, SRPM, "lto-ltfs-0.1.0.tar.gz", "SOURCE-MANIFEST.json",
    "RPM-PAYLOAD-DIGEST", "SHA256SUMS",
}


class PublicBuildTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.api = runpy.run_path(str(VERIFY))

    def test_tag_commit_and_clean_tree_are_required(self):
        with tempfile.TemporaryDirectory() as raw:
            repo = Path(raw)
            def git(*args):
                return subprocess.run(
                    ["git", *args], cwd=repo, check=True, capture_output=True,
                    text=True,
                ).stdout.strip()
            git("init", "-q")
            (repo / "source.txt").write_text("one\n", encoding="utf-8")
            git("add", "source.txt")
            git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                "commit", "-qm", "source")
            commit = git("rev-parse", "HEAD")
            git("tag", "v0.1.0")
            check = self.api["verify_source_ref"]
            error = self.api["PublicDriverBuildError"]
            self.assertGreater(check(repo, "v0.1.0", commit), 0)
            tar_bytes = subprocess.run(
                ["git", "archive", "--format=tar", "--prefix=lto-ltfs-0.1.0/", commit],
                cwd=repo, check=True, capture_output=True,
            ).stdout
            archive = repo / "tag.tar.gz"
            archive.write_bytes(gzip.compress(tar_bytes, mtime=0))
            self.api["verify_tag_archive"](repo, commit, archive)
            archive.write_bytes(gzip.compress(b"different archive", mtime=0))
            with self.assertRaises(error):
                self.api["verify_tag_archive"](repo, commit, archive)
            archive.unlink()
            with self.assertRaises(error):
                check(repo, "v0.1.0", "0" * 40)
            with self.assertRaises(error):
                check(repo, "v0.2.0", commit)
            (repo / "untracked").write_text("dirty", encoding="utf-8")
            with self.assertRaises(error):
                check(repo, "v0.1.0", commit)

    def test_output_allowlist_and_two_build_byte_comparison(self):
        check = self.api["verify_output_set"]
        compare = self.api["compare_builds"]
        error = self.api["PublicDriverBuildError"]
        with tempfile.TemporaryDirectory() as raw:
            first, second = Path(raw) / "first", Path(raw) / "second"
            first.mkdir()
            second.mkdir()
            for name in ARTIFACTS:
                (first / name).write_bytes(name.encode("ascii"))
                (second / name).write_bytes(name.encode("ascii"))
            check(first)
            compare(first, second)
            (second / RPM).write_bytes(b"changed")
            with self.assertRaises(error):
                compare(first, second)
            (second / RPM).write_bytes(RPM.encode("ascii"))
            (first / "extra").write_bytes(b"extra")
            with self.assertRaises(error):
                check(first)

    def test_srpm_archive_must_equal_tag_archive(self):
        check = self.api["verify_srpm_source_bytes"]
        error = self.api["PublicDriverBuildError"]
        with tempfile.TemporaryDirectory() as raw:
            trusted = Path(raw) / "trusted.tar.gz"
            extracted = Path(raw) / "extracted.tar.gz"
            trusted.write_bytes(b"tag archive")
            extracted.write_bytes(b"tag archive")
            check(trusted, extracted)
            extracted.write_bytes(b"different archive")
            with self.assertRaises(error):
                check(trusted, extracted)

    def test_installed_license_and_conditional_inventory_match_tag(self):
        check = self.api["verify_installed_notices"]
        error = self.api["PublicDriverBuildError"]
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            source = root / "source"
            payload = root / "payload"
            inventory = [{
                "path": f"file-{index}",
                "review_status": "policy_approved",
                "origin_certainty": "unverified",
                "approval_basis": "owner-selected conservative LGPL-2.1-only policy",
            } for index in range(7)]
            (source / "provenance").mkdir(parents=True)
            license_root = payload / "usr/share/licenses/lto-ltfs"
            doc_root = payload / "usr/share/doc/lto-ltfs"
            license_root.mkdir(parents=True)
            doc_root.mkdir(parents=True)
            for name in ("COPYING.LIB", "LGPL-NOTICE"):
                (source / name).write_bytes(name.encode())
                (license_root / name).write_bytes(name.encode())
            data = (json.dumps(inventory) + "\n").encode()
            (source / "provenance/license-inventory.json").write_bytes(data)
            (doc_root / "license-inventory.json").write_bytes(data)
            check(source, payload)
            (license_root / "LGPL-NOTICE").write_bytes(b"substitute")
            with self.assertRaises(error):
                check(source, payload)
            (license_root / "LGPL-NOTICE").write_bytes(b"LGPL-NOTICE")
            inventory.pop()
            (doc_root / "license-inventory.json").write_text(json.dumps(inventory))
            with self.assertRaises(error):
                check(source, payload)
            (doc_root / "license-inventory.json").unlink()
            with self.assertRaises(error):
                check(source, payload)

    def test_active_spec_installs_inventory_as_documentation(self):
        spec = (ROOT / "packaging/rpm/lto-ltfs.spec").read_text(encoding="utf-8")
        self.assertIn("%doc provenance/license-inventory.json", spec)


if __name__ == "__main__":
    unittest.main()
