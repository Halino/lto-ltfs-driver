# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import json
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
VENDOR_ROOT = ROOT / "src/libltfs/uthash_submodule"


class VendoredUthashTests(unittest.TestCase):
    def temporary_source(self):
        temporary = tempfile.TemporaryDirectory()
        clone = Path(temporary.name) / "source"
        vendor = clone / "src/libltfs/uthash_submodule"
        vendor.parent.mkdir(parents=True)
        shutil.copytree(VENDOR_ROOT, vendor)
        provenance = clone / "provenance"
        provenance.mkdir()
        shutil.copy2(ROOT / "provenance/uthash.json", provenance / "uthash.json")
        self.addCleanup(temporary.cleanup)
        return clone

    def run_check(self, root):
        return subprocess.run(
            [sys.executable, "scripts/check-vendored-uthash.py", str(root)],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    def test_header_needed_by_ltfs_is_present(self):
        self.assertTrue(
            (VENDOR_ROOT / "src/uthash.h").is_file(),
            "the pinned uthash build dependency is missing",
        )

    def test_metadata_pins_official_commit_files_and_license(self):
        metadata_path = ROOT / "provenance/uthash.json"
        self.assertTrue(metadata_path.is_file(), "vendored uthash metadata is missing")
        metadata = json.loads(metadata_path.read_text())
        self.assertEqual(metadata["repository"], "https://github.com/troydhanson/uthash.git")
        self.assertEqual(metadata["commit"], "2031adfd8cd6f8f498e0f4a9055648b19496f12e")
        self.assertEqual(metadata["license"], "BSD-1-Clause")
        result = self.run_check(ROOT)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_missing_vendored_header(self):
        clone = self.temporary_source()
        (clone / "src/libltfs/uthash_submodule/src/uthash.h").unlink()
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vendored uthash file is missing", result.stderr)

    def test_rejects_tampered_vendored_header(self):
        clone = self.temporary_source()
        header = clone / "src/libltfs/uthash_submodule/src/uthash.h"
        header.write_bytes(header.read_bytes() + b"tampered\n")
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vendored uthash file hash differs", result.stderr)

    def test_rejects_missing_vendor_license(self):
        clone = self.temporary_source()
        (clone / "src/libltfs/uthash_submodule/LICENSE").unlink()
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vendored uthash file is missing", result.stderr)

    def test_rejects_vendor_metadata_drift(self):
        clone = self.temporary_source()
        metadata_path = clone / "provenance/uthash.json"
        metadata = json.loads(metadata_path.read_text())
        metadata["commit"] = "0" * 40
        metadata_path.write_text(json.dumps(metadata, indent=2) + "\n")
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vendored uthash metadata differs", result.stderr)


if __name__ == "__main__":
    unittest.main()
