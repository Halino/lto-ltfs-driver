"""Fail-closed checks for the tag-derived, unsigned public driver build."""

from __future__ import annotations

import json
import gzip
import hashlib
import io
import runpy
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFY = ROOT / "scripts/verify-public-build.py"
RPM = "lto-ltfs-0.1.1-22.el9.x86_64.rpm"
SRPM = "lto-ltfs-0.1.1-22.el9.src.rpm"
ARTIFACTS = {
    RPM, SRPM, "lto-ltfs-0.1.1.tar.gz", "SOURCE-MANIFEST.json",
    "RPM-PAYLOAD-DIGEST", "SHA256SUMS", "BUILD-INPUTS.json",
}


class RpmbuildOutputAdmissionTests(unittest.TestCase):
    """Run the production shell gate against real rpmbuild directory layouts."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name)
        self.binary = self.build / "RPMS/x86_64" / RPM
        self.source_rpm = self.build / "SRPMS" / SRPM
        for path in (self.binary, self.source_rpm):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"package fixture")

    def admit(self):
        # Stop before copying or RPM inspection: this exercises the exact gate,
        # not a reimplementation of its path/count or symlink checks.
        script = (ROOT / "scripts/build-public-unsigned.sh").read_text()
        start = script.index("binary=$build/RPMS/")
        end = script.index('cp -- "$binary" "$source_rpm" "$stage/"', start)
        return subprocess.run(
            ["bash", "-eu", "-o", "pipefail", "-c",
             'build=$1\n' + script[start:end], "output-admission", str(self.build)],
            capture_output=True, text=True,
        )

    def test_accepts_exact_binary_and_source_in_rpmbuild_layout(self):
        result = self.admit()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_extra_package(self):
        (self.binary.parent / "extra.rpm").write_bytes(b"unexpected package")
        result = self.admit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("extra or unexpected package files", result.stderr)

    def test_rejects_missing_source_package(self):
        self.source_rpm.unlink()
        result = self.admit()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exact release-22 RPM/SRPM was not produced", result.stderr)

    def test_rejects_binary_in_wrong_architecture_directory(self):
        wrong = self.build / "RPMS/aarch64" / RPM
        wrong.parent.mkdir()
        self.binary.rename(wrong)
        self.assertNotEqual(self.admit().returncode, 0)

    def test_rejects_wrong_binary_filename(self):
        self.binary.rename(self.binary.with_name("different-release.x86_64.rpm"))
        self.assertNotEqual(self.admit().returncode, 0)

    def test_rejects_binary_symlink(self):
        target = self.build / "binary-target"
        self.binary.rename(target)
        self.binary.symlink_to(target)
        self.assertNotEqual(self.admit().returncode, 0)

    def test_rejects_source_package_symlink(self):
        target = self.build / "source-target"
        self.source_rpm.rename(target)
        self.source_rpm.symlink_to(target)
        self.assertNotEqual(self.admit().returncode, 0)


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
            git("tag", "v0.1.1")
            check = self.api["verify_source_ref"]
            error = self.api["PublicDriverBuildError"]
            self.assertGreater(check(repo, "v0.1.1", commit), 0)
            with self.assertRaises(error):
                check(repo, "v0.1.0", commit)
            tar_bytes = subprocess.run(
                ["git", "archive", "--format=tar", "--prefix=lto-ltfs-0.1.1/", commit],
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
                check(repo, "v0.1.1", "0" * 40)
            with self.assertRaises(error):
                check(repo, "v0.2.0", commit)
            (repo / "untracked").write_text("dirty", encoding="utf-8")
            with self.assertRaises(error):
                check(repo, "v0.1.1", commit)

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

    def test_spec_normalizes_variable_build_root_in_debug_objects(self):
        spec = (ROOT / "packaging/rpm/lto-ltfs.spec").read_text(encoding="utf-8")
        self.assertIn("-ffile-prefix-map=%{_topdir}=/usr/src/debug/lto-ltfs", spec)
        self.assertIn('CFLAGS="%{build_cflags} -ffile-prefix-map=', spec)
        self.assertIn('CXXFLAGS="%{build_cxxflags} -ffile-prefix-map=', spec)
        self.assertIn('LDFLAGS="%{build_ldflags} -Wl,--build-id=none"', spec)

    def test_unsigned_build_requires_verified_icu_tools(self):
        lock = runpy.run_path(str(ROOT / "scripts/prepare-icu-build-tools.py"))["load_icu_lock"](
            ROOT / "packaging/rpm/icu-build-tools.json"
        )
        check = self.api["verify_icu_tools"]
        error = self.api["PublicDriverBuildError"]
        with tempfile.TemporaryDirectory() as raw:
            tools = Path(raw)
            with self.assertRaises(error):
                check(lock, tools)
            fixture = json.loads(json.dumps(lock))
            for row in fixture["payload_members"]:
                if row[0] in fixture["tool_members"]:
                    data = row[0].encode()
                    (tools / Path(row[0]).name).write_bytes(data)
                    (tools / Path(row[0]).name).chmod(0o555)
                    row[2] = hashlib.sha256(data).hexdigest()
            check(fixture, tools)
            (tools / "pkgdata").chmod(0o755)
            (tools / "pkgdata").write_bytes(b"changed")
            (tools / "pkgdata").chmod(0o555)
            with self.assertRaises(error):
                check(fixture, tools)
            (tools / "extra").write_bytes(b"extra")
            with self.assertRaises(error):
                check(fixture, tools)

    def test_distributed_payload_excludes_provider(self):
        check = self.api["reject_provider_files"]
        check_archive = self.api["reject_provider_archive"]
        error = self.api["PublicDriverBuildError"]
        provider = b"CentOS ICU build executable"
        digest = hashlib.sha256(provider).hexdigest()
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            (root / "ltfs").write_bytes(b"legitimate driver payload")
            check(root, {digest})
            (root / "genrb").write_bytes(provider)
            with self.assertRaises(error):
                check(root, {digest})
            (root / "genrb").unlink()
            (root / "libicuuc.so.67").write_bytes(b"unreviewed library")
            with self.assertRaises(error):
                check(root, {digest})
            archive = root / "source.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                item = tarfile.TarInfo("lto-ltfs-0.1.1/ordinary-name")
                item.size = len(provider)
                output.addfile(item, io.BytesIO(provider))
            with self.assertRaises(error):
                check_archive(archive, {digest})

    def test_two_builds_reject_catalog_byte_difference(self):
        compare = self.api["compare_builds"]
        error = self.api["PublicDriverBuildError"]
        with tempfile.TemporaryDirectory() as raw:
            first, second = Path(raw) / "first", Path(raw) / "second"
            first.mkdir()
            second.mkdir()
            for name in ARTIFACTS:
                (first / name).write_bytes(name.encode())
                (second / name).write_bytes(name.encode())
            (second / RPM).write_bytes(RPM.encode() + b"one catalog byte changed")
            with self.assertRaises(error):
                compare(first, second)

    def test_comparison_cli_refuses_mutated_or_extra_artifacts(self):
        with tempfile.TemporaryDirectory() as raw:
            first, second = Path(raw) / "first", Path(raw) / "second"
            first.mkdir()
            second.mkdir()
            for root in (first, second):
                for name in ARTIFACTS - {"SHA256SUMS"}:
                    (root / name).write_bytes(name.encode())
                (root / "SHA256SUMS").write_text("".join(
                    f"{hashlib.sha256((root / name).read_bytes()).hexdigest()}  {name}\n"
                    for name in sorted(ARTIFACTS - {"SHA256SUMS"})
                ), encoding="ascii")
            command = ["python3", str(VERIFY), "--compare-first", str(first),
                       "--compare-second", str(second)]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
            (second / RPM).write_bytes(b"one byte different")
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
            (second / RPM).write_bytes(RPM.encode())
            (second / "extra").write_bytes(b"unapproved")
            self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)

    def test_container_uses_icu_tools_only_in_build_stage(self):
        container = (ROOT / "packaging/rpm/Containerfile").read_text()
        build_stage, runtime_stage = container.split(" AS runtime-test", 1)
        self.assertIn("COPY icu-tools /workspace/icu-tools", build_stage)
        self.assertIn("verify-icu-catalogs.sh", build_stage)
        self.assertIn("BUILD-INPUTS.json", build_stage)
        self.assertNotIn("COPY icu-tools", runtime_stage)
        self.assertNotIn("COPY --from=build /workspace/icu-tools", runtime_stage)

    def test_builder_reauthenticates_bundle_before_admitting_bootstrap_cpio(self):
        script = (ROOT / "scripts/build-public-unsigned.sh").read_text()
        self.assertLess(script.index("--authenticate-bundle"), script.index("LTO_CPIO_PREINSTALLED"))
        self.assertIn('rpm -V cpio', script)
        self.assertIn('cpio-2.13-16.el9.x86_64.rpm', script)
        self.assertNotIn("--replacepkgs", script)

    def test_build_input_manifest_records_identity_not_provider_bytes(self):
        lock = runpy.run_path(str(ROOT / "scripts/prepare-icu-build-tools.py"))["load_icu_lock"](
            ROOT / "packaging/rpm/icu-build-tools.json"
        )
        manifest = self.api["build_inputs_bytes"](lock)
        parsed = json.loads(manifest)
        self.assertEqual(parsed["provider"]["sha256"], lock["sha256"])
        self.assertEqual(set(parsed["build_tools"]), {"genrb", "pkgdata"})
        self.assertIs(parsed["runtime_provider_bytes"], False)
        self.assertLess(len(manifest), 2048)

    def test_exact_disclosed_license_inventory_survives_vendor_scan(self):
        verifier = runpy.run_path(str(ROOT / "scripts/verify-rpm.py"))
        path = "/usr/share/doc/lto-ltfs/license-inventory.json"
        contents = (ROOT / "provenance/license-inventory.json").read_bytes().decode("latin-1")
        verifier["verify_text"](path, contents)
        with self.assertRaises(verifier["VerificationError"]):
            verifier["verify_text"](path, contents + "\nextra HPE claim")


if __name__ == "__main__":
    unittest.main()
