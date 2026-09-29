import hashlib
import json
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CHECK = ROOT / "scripts/check-public-license-inventory.py"


class PublicLicenseInventoryTests(unittest.TestCase):
    def test_committed_inventory_matches_every_reviewed_source_file(self):
        inventory = json.loads((ROOT / "provenance/license-inventory.json").read_text())
        overlays = json.loads((ROOT / "provenance/downstream-overlays.json").read_text())
        by_path = {row["path"]: row for row in inventory}
        self.assertEqual(set(by_path), {row["path"] for row in overlays})
        for path, row in by_path.items():
            with self.subTest(path=path):
                actual = hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
                self.assertEqual(row["sha256"], actual)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "src/vendor").mkdir(parents=True)
        (self.root / "src/component.c").write_text(
            "/* SPDX-License-Identifier: GPL-2.0-only */\nint component(void) { return 0; }\n"
        )
        (self.root / "src/vendor/uthash.h").write_text(
            "/* SPDX-License-Identifier: BSD-1-Clause */\n"
        )
        paths = ("src/component.c", "src/vendor/uthash.h")
        (self.root / "overlays.json").write_text(
            json.dumps([
                {
                    "path": path,
                    "downstream_sha256": hashlib.sha256((self.root / path).read_bytes()).hexdigest(),
                }
                for path in paths
            ])
        )
        self.archive = self.root / "source.tar.gz"
        with tarfile.open(self.archive, "w:gz") as tar:
            for path in paths:
                tar.add(self.root / path, arcname=path)
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def check(self, inventory, package_license, digest=None):
        (self.root / "inventory.json").write_text(json.dumps(inventory))
        return subprocess.run(
            [
                sys.executable, str(CHECK), "--root", str(self.root),
                "--source-archive", str(self.archive),
                "--source-sha256", digest or self.digest,
                "--overlays", str(self.root / "overlays.json"),
                "--inventory", str(self.root / "inventory.json"),
                "--package-license", package_license,
            ],
            capture_output=True, text=True,
        )

    def reviewed_inventory(self):
        return [
            {
                "path": path,
                "sha256": hashlib.sha256((self.root / path).read_bytes()).hexdigest(),
                "license": license_id,
                "origin": "fixture-owned",
                "evidence": "synthetic fixture reviewed by this test",
                "review_status": "reviewed",
            }
            for path, license_id in (
                ("src/component.c", "GPL-2.0-only"),
                ("src/vendor/uthash.h", "BSD-1-Clause"),
            )
        ]

    def test_gpl_overlay_cannot_ship_under_bsd_only_package_label(self):
        result = self.check(self.reviewed_inventory(), "BSD-3-Clause")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("GPL-2.0-only", result.stderr)

    def test_wrong_source_archive_digest_blocks_inventory(self):
        result = self.check(
            self.reviewed_inventory(),
            "GPL-2.0-only AND BSD-1-Clause",
            digest="0" * 64,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source archive SHA-256 mismatch", result.stderr)

    def test_uthash_license_must_be_in_package_label(self):
        result = self.check(self.reviewed_inventory(), "GPL-2.0-only")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("BSD-1-Clause", result.stderr)

    def test_reviewed_exact_file_inventory_can_pass(self):
        result = self.check(
            self.reviewed_inventory(), "GPL-2.0-only AND BSD-1-Clause"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ibm_and_lgpl_file_needs_both_package_licenses(self):
        source = self.root / "src/component.c"
        source.write_text(
            "/* SPDX-License-Identifier: BSD-3-Clause AND LGPL-2.1-only */\n"
        )
        overlays = json.loads((self.root / "overlays.json").read_text())
        overlays[0]["downstream_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        (self.root / "overlays.json").write_text(json.dumps(overlays))
        with tarfile.open(self.archive, "w:gz") as archive:
            for path in ("src/component.c", "src/vendor/uthash.h"):
                archive.add(self.root / path, arcname=path)
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        inventory = self.reviewed_inventory()
        inventory[0]["sha256"] = overlays[0]["downstream_sha256"]
        inventory[0]["license"] = "BSD-3-Clause AND LGPL-2.1-only"

        result = self.check(
            inventory, "BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        result = self.check(inventory, "BSD-3-Clause AND BSD-1-Clause")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LGPL-2.1-only", result.stderr)

        inventory[0]["review_status"] = "policy_approved"
        inventory[0]["origin_certainty"] = "unverified"
        inventory[0]["approval_basis"] = "owner-selected conservative LGPL-2.1-only policy"
        result = self.check(
            inventory, "BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        del inventory[0]["approval_basis"]
        result = self.check(
            inventory, "BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("approval", result.stderr.lower())
    def test_archive_bytes_must_match_reviewed_tree(self):
        source = self.root / "src/component.c"
        source.write_text("/* SPDX-License-Identifier: GPL-2.0-only */\nint changed(void) { return 1; }\n")
        overlays = json.loads((self.root / "overlays.json").read_text())
        overlays[0]["downstream_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        (self.root / "overlays.json").write_text(json.dumps(overlays))
        result = self.check(
            self.reviewed_inventory(), "GPL-2.0-only AND BSD-1-Clause"
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("archive content differs", result.stderr)

    def test_unreviewed_overlay_blocks_even_with_a_matching_header(self):
        inventory = self.reviewed_inventory()
        inventory[0]["review_status"] = "unresolved"
        result = self.check(inventory, "GPL-2.0-only AND BSD-1-Clause")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unreviewed", result.stderr)

    def test_compound_spdx_header_cannot_be_reduced_to_first_license(self):
        source = self.root / "src/component.c"
        source.write_text("/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only */\n")
        overlays = json.loads((self.root / "overlays.json").read_text())
        overlays[0]["downstream_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
        (self.root / "overlays.json").write_text(json.dumps(overlays))
        with tarfile.open(self.archive, "w:gz") as archive:
            for path in ("src/component.c", "src/vendor/uthash.h"):
                archive.add(self.root / path, arcname=path)
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()

        inventory = self.reviewed_inventory()
        inventory[0]["license"] = "BSD-3-Clause"
        result = self.check(inventory, "BSD-3-Clause AND BSD-1-Clause")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("compound SPDX", result.stderr)

    def test_parent_symlink_cannot_escape_reviewed_tree(self):
        vendor = self.root / "src/vendor"
        original = self.root / "original-vendor"
        vendor.rename(original)
        vendor.symlink_to(original, target_is_directory=True)
        result = self.check(
            self.reviewed_inventory(), "GPL-2.0-only AND BSD-1-Clause"
        )
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
