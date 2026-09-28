# SPDX-License-Identifier: BSD-3-Clause

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "scripts" / "verify-test-surface.py"
REAL_BUILD = Path(os.environ.get("LTFS_BUILD_DIR", ROOT))
REAL_MANIFEST = ROOT / "qualification" / "ltfs-surface-v1.json"


class SurfaceVerifierTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.source.mkdir()
        self.build.mkdir()
        (self.source / "tests").mkdir()
        (self.source / "tests" / "test_evidence.py").write_text(
            "def test_alpha_success():\n    return ltfs_init()\n"
            "def test_wipe_refusal():\n    return tape_unformat()\n"
            "def test_wipe_success():\n    return tape_format()\n"
            "def test_unrelated_success():\n    return unrelated_symbol()\n",
            encoding="utf-8",
        )
        (self.source / "tests" / "test_evidence.c").write_text(
            "static int test_c_boundary(void) { return ltfs_mount(); }\n"
            "static int test_c_init(void) { return ltfs_init(); }\n",
            encoding="utf-8",
        )
        self._write_cli("ltfs", ("--alpha", "--wipe"))
        self._build_library(("ltfs_init", "ltfs_mount"))

    def _write_cli(
        self,
        name,
        options,
        *,
        expected="--advanced-help",
        second=None,
        expected_ld=None,
        exit_code=0,
    ):
        binary = self.build / name
        rendered = "\n".join(options)
        second_check = f'test "$2" = {second} || exit 65\n' if second else ""
        loader_check = (
            f'test "$LD_LIBRARY_PATH" = {expected_ld} || exit 66\n'
            if expected_ld
            else ""
        )
        binary.write_text(
            "#!/bin/sh\n"
            f'test "$1" = {expected} || exit 64\n'
            f"{second_check}"
            f"{loader_check}"
            f"printf '%s\\n' '{rendered}' >&2\n"
            f"exit {exit_code}\n",
            encoding="utf-8",
        )
        binary.chmod(0o755)

    def _build_library(self, symbols):
        source = self.build / "surface.c"
        source.write_text(
            "\n".join(f"int {symbol}(void) {{ return 0; }}" for symbol in symbols),
            encoding="utf-8",
        )
        subprocess.run(
            [
                "gcc",
                "-shared",
                "-fPIC",
                "-o",
                str(self.build / "libltfs.so"),
                str(source),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def _manifest(self):
        return {
            "schema": 2,
            "library": "libltfs.so",
            "commands": {
                "ltfs": {
                    "artifact": "ltfs",
                    "help_argv": ["--advanced-help"],
                    "help_exit_codes": [0],
                    "options": [
                        {
                            "name": "--alpha",
                            "operation_class": "read-only",
                            "evidence": ["tests/test_evidence.py::test_alpha_success"],
                            "physical_policy": None,
                        },
                        {
                            "name": "--wipe",
                            "operation_class": "destructive",
                            "evidence": [
                                "tests/test_evidence.py::test_wipe_refusal",
                                "tests/test_evidence.py::test_wipe_success",
                            ],
                            "physical_policy": "operation-token",
                        },
                    ],
                }
            },
            "exports": [
                {
                    "name": "ltfs_init",
                    "operation_class": "read-only",
                    "evidence": ["tests/test_evidence.py::test_alpha_success"],
                    "probe": "source-call",
                    "physical_policy": None,
                },
                {
                    "name": "ltfs_mount",
                    "operation_class": "positioning",
                    "evidence": ["tests/test_evidence.c::test_c_boundary"],
                    "probe": "source-call",
                    "physical_policy": None,
                },
            ],
        }

    def _run(self, manifest, *, environment=None):
        path = self.root / "surface.json"
        path.write_text(json.dumps(manifest), encoding="utf-8")
        return subprocess.run(
            [
                os.fspath(VERIFIER),
                "--manifest",
                os.fspath(path),
                "--source-root",
                os.fspath(self.source),
                "--build-root",
                os.fspath(self.build),
            ],
            text=True,
            capture_output=True,
            check=False,
            env=environment,
        )

    def test_accepts_exact_closed_surface(self):
        result = self._run(self._manifest())
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            "surface verification passed: 2 options, 2 exports "
            "(2 source-call, 0 ABI-only)\n",
            result.stdout,
        )

    def test_json_result_keeps_abi_and_source_call_counts_distinct(self):
        manifest = self.root / "surface-json.json"
        manifest.write_text(json.dumps(self._manifest()), encoding="utf-8")
        result = subprocess.run(
            [
                os.fspath(VERIFIER),
                "--manifest",
                os.fspath(manifest),
                "--source-root",
                os.fspath(self.source),
                "--build-root",
                os.fspath(self.build),
                "--json",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            {
                "schema": 1,
                "verdict": "PASS",
                "options": 2,
                "exports": {
                    "total": 2,
                    "source_call": 2,
                    "abi_export": 0,
                },
            },
            json.loads(result.stdout),
        )

    def test_rejects_unmapped_exported_symbol(self):
        manifest = self._manifest()
        manifest["exports"].pop()
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("unmapped exported surface: ltfs_mount", result.stderr)

    def test_rejects_destructive_surface_without_policy(self):
        manifest = self._manifest()
        manifest["commands"]["ltfs"]["options"][1]["physical_policy"] = None
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("destructive surface requires a physical policy", result.stderr)

    def test_rejects_missing_failure_sensitive_evidence(self):
        manifest = self._manifest()
        manifest["commands"]["ltfs"]["options"][1]["evidence"] = [
            "tests/test_evidence.py::test_wipe_success"
        ]
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("destructive surface requires refusal evidence", result.stderr)

    def test_rejects_existing_test_that_does_not_call_the_export(self):
        manifest = self._manifest()
        manifest["exports"][0]["evidence"] = [
            "tests/test_evidence.py::test_unrelated_success"
        ]
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("export evidence does not call symbol: ltfs_init", result.stderr)

    def test_accepts_honest_abi_only_export_without_behavior_claim(self):
        manifest = self._manifest()
        manifest["exports"][0]["probe"] = "abi-export"
        manifest["exports"][0]["evidence"] = []
        result = self._run(manifest)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("1 source-call, 1 ABI-only", result.stdout)

    def test_rejects_incoherent_or_unknown_export_probe(self):
        manifest = self._manifest()
        manifest["exports"][0]["probe"] = "abi-export"
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("ABI-only export must not claim test evidence", result.stderr)

        manifest = self._manifest()
        manifest["exports"][0]["evidence"] = []
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("source-call export requires test evidence", result.stderr)

        manifest = self._manifest()
        manifest["exports"][0]["probe"] = "runtime-trace"
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("export probe is invalid", result.stderr)

    def test_rejects_read_only_class_for_known_media_mutators(self):
        mutators = {
            "ltfs_format_tape": "destructive",
            "ltfs_unformat_tape": "destructive",
            "ltfs_eject_tape": "destructive",
            "ltfs_reset_capacity": "destructive",
            "tape_reset_capacity": "destructive",
            "set_tape_attribute": "write",
            "tape_set_attribute_to_cm": "write",
            "tape_set_key": "write",
            "tape_clear_key": "write",
            "dcache_setxattr": "write",
            "ltfs_fsops_setxattr": "write",
            "ltfs_set_vendorunique_xattr": "write",
            "tape_set_vendorunique_xattr": "write",
            "xattr_do_set": "write",
            "xattr_set": "write",
        }
        for symbol, required_class in mutators.items():
            with self.subTest(symbol=symbol):
                self._build_library((symbol, "ltfs_mount"))
                manifest = self._manifest()
                manifest["exports"][0]["name"] = symbol
                manifest["exports"][0]["probe"] = "abi-export"
                manifest["exports"][0]["evidence"] = []
                result = self._run(manifest)
                self.assertEqual(2, result.returncode)
                self.assertIn(
                    f"export {symbol} requires {required_class} operation class",
                    result.stderr,
                )

    def test_rejects_unknown_keys_and_duplicate_evidence(self):
        manifest = self._manifest()
        manifest["commands"]["ltfs"]["extra"] = True
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("unknown manifest key", result.stderr)

        manifest = self._manifest()
        evidence = manifest["exports"][0]["evidence"][0]
        manifest["exports"][0]["evidence"].append(evidence)
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("duplicate evidence", result.stderr)

    def test_rejects_cli_option_drift_and_missing_test(self):
        manifest = self._manifest()
        manifest["commands"]["ltfs"]["options"][0]["name"] = "--beta"
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("unmapped CLI option: ltfs:--alpha", result.stderr)

        manifest = self._manifest()
        manifest["exports"][0]["evidence"] = ["tests/test_evidence.py::test_absent"]
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("evidence test is absent", result.stderr)

    def test_rejects_symlinked_artifacts_and_noncanonical_roots(self):
        manifest = self._manifest()
        target = self.build / "ltfs"
        renamed = self.build / "ltfs.real"
        target.rename(renamed)
        target.symlink_to(renamed)
        result = self._run(manifest)
        self.assertEqual(2, result.returncode)
        self.assertIn("artifact must be a regular non-symlink file", result.stderr)

    def test_accepts_usage_exit_and_fuse_style_options(self):
        self._write_cli(
            "ltfs-info",
            ("--json", "-o eject", "-o standalone_receipt=<path>"),
            expected="--help",
            exit_code=2,
        )
        manifest = self._manifest()
        manifest["commands"]["ltfs-info"] = {
            "artifact": "ltfs-info",
            "help_argv": ["--help"],
            "help_exit_codes": [2],
            "options": [
                {
                    "name": "--json",
                    "operation_class": "read-only",
                    "evidence": ["tests/test_evidence.py::test_alpha_success"],
                    "physical_policy": None,
                },
                {
                    "name": "-o:eject",
                    "operation_class": "destructive",
                    "evidence": [
                        "tests/test_evidence.py::test_wipe_refusal",
                        "tests/test_evidence.py::test_wipe_success",
                    ],
                    "physical_policy": "operation-token",
                },
                {
                    "name": "-o:standalone_receipt",
                    "operation_class": "commit",
                    "evidence": ["tests/test_evidence.py::test_alpha_success"],
                    "physical_policy": None,
                },
            ],
        }
        result = self._run(manifest)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            "surface verification passed: 5 options, 2 exports "
            "(2 source-call, 0 ABI-only)\n",
            result.stdout,
        )

    def test_accepts_c_test_evidence(self):
        manifest = self._manifest()
        manifest["exports"][0]["evidence"] = [
            "tests/test_evidence.c::test_c_init"
        ]
        result = self._run(manifest)
        self.assertEqual(0, result.returncode, result.stderr)

    def test_expands_canonical_source_root_in_help_argv(self):
        config = self.source / "surface.conf"
        config.write_text("closed fixture\n", encoding="utf-8")
        self._write_cli(
            "ltfs",
            ("--alpha", "--wipe"),
            expected="--config",
            second=str(config),
        )
        manifest = self._manifest()
        manifest["commands"]["ltfs"]["help_argv"] = [
            "--config",
            "{source_root}/surface.conf",
        ]
        result = self._run(manifest)
        self.assertEqual(0, result.returncode, result.stderr)

    def test_passes_the_existing_dynamic_loader_path_to_cli_help(self):
        loader_path = "/closed/loader/path"
        self._write_cli(
            "ltfs", ("--alpha", "--wipe"), expected_ld=loader_path
        )
        environment = dict(os.environ)
        environment["LD_LIBRARY_PATH"] = loader_path
        result = self._run(self._manifest(), environment=environment)
        self.assertEqual(0, result.returncode, result.stderr)

    def test_prepends_the_configured_build_library_path_to_cli_help(self):
        library_directory = self.build / "src" / "libltfs" / ".libs"
        library_directory.mkdir(parents=True)
        inherited = "/closed/loader/path"
        expected = f"{library_directory}{os.pathsep}{inherited}"
        self._write_cli("ltfs", ("--alpha", "--wipe"), expected_ld=expected)
        environment = dict(os.environ)
        environment["LD_LIBRARY_PATH"] = inherited
        result = self._run(self._manifest(), environment=environment)
        self.assertEqual(0, result.returncode, result.stderr)

    @unittest.skipUnless(
        (REAL_BUILD / "src" / "libltfs" / ".libs" / "libltfs.so.0.0.0").is_file(),
        "configured real LTFS build is unavailable",
    )
    def test_repository_manifest_matches_the_real_build(self):
        result = subprocess.run(
            [
                os.fspath(VERIFIER),
                "--manifest",
                os.fspath(REAL_MANIFEST),
                "--source-root",
                os.fspath(ROOT),
                "--build-root",
                os.fspath(REAL_BUILD),
            ],
            text=True,
            capture_output=True,
            check=False,
            env=dict(os.environ),
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(
            "surface verification passed: 136 options, 606 exports "
            "(56 source-call, 550 ABI-only)\n",
            result.stdout,
        )

    def test_repository_surface_assets_are_in_the_source_distribution(self):
        makefile = (ROOT / "Makefile.am").read_text(encoding="utf-8")
        for relative in (
            "qualification/ltfs-surface-v1.json",
            "qualification/ltfs-surface.conf",
            "qualification/run-file-destructive.py",
            "scripts/verify-test-surface.py",
        ):
            with self.subTest(relative=relative):
                self.assertIn(relative, makefile)


if __name__ == "__main__":
    unittest.main()
