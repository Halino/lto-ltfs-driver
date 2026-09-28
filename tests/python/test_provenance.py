import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
UPSTREAM_FIXTURE = Path(
    os.environ.get("LTFS_PROVENANCE_UPSTREAM", "/tmp/ltfs-upstream-plan")
)
class ProvenanceTests(unittest.TestCase):
    def manifest_entries(self, root):
        entries = {}
        for line in (root / "provenance/upstream-files.sha256").read_text().splitlines():
            digest, path = line.split("  ", 1)
            entries[path] = digest
        return entries

    def valid_overlay_entries(self, root):
        manifest = self.manifest_entries(root)
        entries = []
        overlay_paths = [
            entry["path"]
            for entry in json.loads(
                (root / "provenance/downstream-overlays.json").read_text()
            )
        ]
        for path in overlay_paths:
            source = root / path
            entries.append(
                {
                    "path": path,
                    "upstream_sha256": manifest.get(path),
                    "downstream_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "rationale": "Document the intentional downstream LTFS foundation change.",
                    "task": (
                        "task-1"
                        if path.startswith("docs/")
                        else "task-3"
                        if path.startswith("src/libltfs/ltfs_events")
                        or path == "src/libltfs/Makefile.am"
                        else "task-2"
                    ),
                }
            )
        return sorted(entries, key=lambda entry: entry["path"])

    def write_overlay(self, root, entries):
        (root / "provenance/downstream-overlays.json").write_text(
            json.dumps(entries, indent=2) + "\n"
        )

    def run_check(self, root, *extra_args, environment=None):
        env = os.environ.copy()
        if environment:
            env.update(environment)
        return subprocess.run(
            [sys.executable, "scripts/check-provenance.py", str(root), *extra_args],
            cwd=ROOT,
            env=env,
            capture_output=True,
            text=True,
        )

    def temporary_import(self):
        temporary = tempfile.TemporaryDirectory()
        clone = Path(temporary.name) / "import"
        shutil.copytree(
            ROOT,
            clone,
            ignore=shutil.ignore_patterns(".git", "__pycache__"),
        )
        self.addCleanup(temporary.cleanup)
        return clone

    def upstream_args(self):
        if not (UPSTREAM_FIXTURE / ".git").exists():
            if os.environ.get("CI"):
                self.fail("CI must provide LTFS_PROVENANCE_UPSTREAM for the full tree oracle")
            self.skipTest("set LTFS_PROVENANCE_UPSTREAM to an exact local upstream clone")
        return ("--upstream", str(UPSTREAM_FIXTURE))

    def temporary_upstream(self):
        self.upstream_args()
        temporary = tempfile.TemporaryDirectory()
        clone = Path(temporary.name) / "upstream"
        subprocess.run(
            ["git", "clone", "--no-checkout", "--shared", str(UPSTREAM_FIXTURE), str(clone)],
            check=True,
            capture_output=True,
            text=True,
        )
        self.addCleanup(temporary.cleanup)
        return clone

    def test_pinned_upstream_identity_and_license(self):
        data = json.loads((ROOT / "provenance/upstream.json").read_text())
        self.assertEqual(data["commit"], "7d0de7c0a71296353160f4c5bc082fec9af04e5c")
        self.assertEqual(data["tag"], "v2.4.8.4-10522")
        self.assertEqual(data["license"], "BSD-3-Clause")
        result = self.run_check(ROOT)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_full_oracle_accepts_exact_local_upstream(self):
        result = self.run_check(ROOT, *self.upstream_args())
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_accepts_registered_modifications_and_additions(self):
        clone = self.temporary_import()
        self.write_overlay(clone, self.valid_overlay_entries(clone))
        result = self.run_check(clone)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_accepts_registered_addition(self):
        clone = self.temporary_import()
        added = clone / "src/downstream-overlay.c"
        added.write_text("/* SPDX-License-Identifier: BSD-3-Clause */\n")
        entries = self.valid_overlay_entries(clone)
        entries.append(
            {
                "path": "src/downstream-overlay.c",
                "upstream_sha256": None,
                "downstream_sha256": hashlib.sha256(added.read_bytes()).hexdigest(),
                "rationale": "Exercise a declared downstream addition.",
                "task": "test",
            }
        )
        self.write_overlay(clone, sorted(entries, key=lambda entry: entry["path"]))
        result = self.run_check(clone)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_accepts_registered_deletion(self):
        clone = self.temporary_import()
        deleted_path = "src/libltfs/base64.c"
        (clone / deleted_path).unlink()
        entries = self.valid_overlay_entries(clone)
        entries.append(
            {
                "path": deleted_path,
                "upstream_sha256": self.manifest_entries(clone)[deleted_path],
                "downstream_sha256": None,
                "rationale": "Exercise a declared downstream deletion.",
                "task": "test",
            }
        )
        self.write_overlay(clone, sorted(entries, key=lambda entry: entry["path"]))
        result = self.run_check(clone)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_duplicate_overlay_path(self):
        clone = self.temporary_import()
        entries = self.valid_overlay_entries(clone)
        entries.append(dict(entries[0]))
        self.write_overlay(clone, entries)
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate", result.stderr)

    def test_rejects_unsorted_overlays(self):
        clone = self.temporary_import()
        self.write_overlay(clone, list(reversed(self.valid_overlay_entries(clone))))
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sorted", result.stderr)

    def test_rejects_wrong_overlay_upstream_hash(self):
        clone = self.temporary_import()
        entries = self.valid_overlay_entries(clone)
        entries[0]["upstream_sha256"] = "0" * 64
        self.write_overlay(clone, entries)
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("upstream hash", result.stderr)

    def test_rejects_wrong_overlay_downstream_hash(self):
        clone = self.temporary_import()
        entries = self.valid_overlay_entries(clone)
        entries[0]["downstream_sha256"] = "0" * 64
        self.write_overlay(clone, entries)
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("downstream hash", result.stderr)

    def test_rejects_unnecessary_overlay(self):
        clone = self.temporary_import()
        entries = self.valid_overlay_entries(clone)
        autogen_hash = self.manifest_entries(clone)["autogen.sh"]
        entries.append(
            {
                "path": "autogen.sh",
                "upstream_sha256": autogen_hash,
                "downstream_sha256": autogen_hash,
                "rationale": "This entry must be rejected as a no-op.",
                "task": "test",
            }
        )
        self.write_overlay(clone, sorted(entries, key=lambda entry: entry["path"]))
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unnecessary", result.stderr)

    def test_rejects_overlay_schema_drift(self):
        clone = self.temporary_import()
        entries = self.valid_overlay_entries(clone)
        entries[0]["unexpected"] = True
        self.write_overlay(clone, entries)
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("schema", result.stderr)

    def test_full_oracle_rejects_absent_pinned_tag(self):
        upstream = self.temporary_upstream()
        subprocess.run(
            ["git", "-C", str(upstream), "tag", "-d", "v2.4.8.4-10522"],
            check=True,
            capture_output=True,
            text=True,
        )
        result = self.run_check(ROOT, "--upstream", str(upstream))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tag", result.stderr)

    def test_full_oracle_rejects_mispointed_pinned_tag(self):
        upstream = self.temporary_upstream()
        other_commit = subprocess.run(
            ["git", "-C", str(upstream), "rev-parse", "7d0de7c0a71296353160f4c5bc082fec9af04e5c^"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout.strip()
        subprocess.run(
            [
                "git",
                "-C",
                str(upstream),
                "update-ref",
                "refs/tags/v2.4.8.4-10522",
                other_commit,
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        result = self.run_check(ROOT, "--upstream", str(upstream))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tag", result.stderr)

    def test_full_oracle_rejects_content_and_manifest_tampering(self):
        clone = self.temporary_import()
        autogen = clone / "autogen.sh"
        autogen.write_bytes(autogen.read_bytes() + b"tampered\n")
        manifest = clone / "provenance/upstream-files.sha256"
        lines = manifest.read_text().splitlines()
        for index, line in enumerate(lines):
            if line.endswith("  autogen.sh"):
                lines[index] = hashlib.sha256(autogen.read_bytes()).hexdigest() + "  autogen.sh"
                break
        manifest.write_text("\n".join(lines) + "\n")
        self.assertEqual(self.run_check(clone).returncode, 0)
        result = self.run_check(clone, *self.upstream_args())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("upstream", result.stderr)

    def test_rejects_omitted_manifest_entry(self):
        clone = self.temporary_import()
        manifest = clone / "provenance/upstream-files.sha256"
        manifest.write_text("\n".join(manifest.read_text().splitlines()[1:]) + "\n")
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("manifest", result.stderr)

    def test_rejects_extra_file_in_import_namespace(self):
        clone = self.temporary_import()
        (clone / "src/unmanifested-source.c").write_text("not upstream\n")
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("boundary", result.stderr)

    def test_rejects_excluded_path(self):
        clone = self.temporary_import()
        excluded = clone / ".github"
        excluded.mkdir()
        (excluded / "workflow.yml").write_text("not upstream\n")
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("excluded", result.stderr)

    def test_rejects_malformed_manifest_path(self):
        clone = self.temporary_import()
        manifest = clone / "provenance/upstream-files.sha256"
        manifest.write_text("0" * 64 + "  /private/path\n" + manifest.read_text())
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unsafe", result.stderr)

    def test_rejects_corrupted_license(self):
        clone = self.temporary_import()
        (clone / "LICENSE").write_text("not the upstream license\n")
        result = self.run_check(clone)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("LICENSE", result.stderr)

    def test_ci_environment_does_not_trigger_uncontrolled_network_access(self):
        result = self.run_check(
            ROOT,
            environment={"CI": "true", "LTFS_PROVENANCE_NETWORK": "1"},
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
