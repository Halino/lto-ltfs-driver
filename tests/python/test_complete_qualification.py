# SPDX-License-Identifier: BSD-3-Clause

import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "scripts" / "run-complete-qualification.sh"
PHASES = (
    "surface-verification",
    "unit-tests",
    "file-backend-destructive",
    "sanitizers",
    "gcov-export",
)


class CompleteQualificationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.build = self.root / "build"
        self.fixtures = self.root / "fixtures"
        self.source.mkdir()
        self.build.mkdir()
        self.fixtures.mkdir()
        self.output = self.root / "result.json"
        self.log = self.root / "phases.log"
        for name in PHASES:
            self._phase(name)

    def _phase(self, name, *, exit_code=0, surface=None):
        path = self.fixtures / name
        payload = ""
        if name == "surface-verification":
            payload = json.dumps(
                surface
                or {
                    "schema": 1,
                    "verdict": "PASS",
                    "options": 130,
                    "exports": {
                        "total": 603,
                        "source_call": 52,
                        "abi_export": 551,
                    },
                },
                separators=(",", ":"),
            )
            payload = f"printf '%s\\n' '{payload}'\n"
        path.write_text(
            "#!/bin/sh\n"
            f"printf '%s\\n' '{name}' >> '{self.log}'\n"
            f"{payload}"
            f"exit {exit_code}\n",
            encoding="utf-8",
        )
        path.chmod(0o755)

    def _run(self):
        self.assertTrue(RUNNER.is_file(), "complete qualification runner is missing")
        return subprocess.run(
            [
                os.fspath(RUNNER),
                "--source-root",
                os.fspath(self.source),
                "--build-root",
                os.fspath(self.build),
                "--output",
                os.fspath(self.output),
                "--test-fixture-dir",
                os.fspath(self.fixtures),
            ],
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )

    def test_runs_closed_hardware_free_gate_and_records_probe_kinds(self):
        result = self._run()
        self.assertEqual(0, result.returncode, result.stderr)
        record = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual("PASS", record["verdict"])
        self.assertIs(record["hardware_free"], True)
        self.assertEqual(
            {"total": 603, "source_call": 52, "abi_export": 551},
            record["surface"]["exports"],
        )
        self.assertEqual(
            list(PHASES),
            [phase["name"] for phase in record["phases"]],
        )
        self.assertTrue(all(phase["status"] == "PASS" for phase in record["phases"]))
        self.assertTrue(
            all(type(phase["duration_seconds"]) is int for phase in record["phases"])
        )
        self.assertEqual(list(PHASES), self.log.read_text(encoding="utf-8").splitlines())

    def test_stops_at_first_failure_and_never_runs_later_destructive_phase(self):
        self._phase("unit-tests", exit_code=17)
        result = self._run()
        self.assertNotEqual(0, result.returncode)
        self.assertEqual(
            ["surface-verification", "unit-tests"],
            self.log.read_text(encoding="utf-8").splitlines(),
        )
        record = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual("FAIL", record["verdict"])
        self.assertEqual(17, record["phases"][-1]["exit_code"])
        self.assertNotIn("file-backend-destructive", self.log.read_text(encoding="utf-8"))

    def test_rejects_surface_result_without_source_call_coverage(self):
        self._phase(
            "surface-verification",
            surface={
                "schema": 1,
                "verdict": "PASS",
                "options": 130,
                "exports": {"total": 603, "source_call": 0, "abi_export": 603},
            },
        )
        result = self._run()
        self.assertNotEqual(0, result.returncode)
        self.assertEqual(["surface-verification"], self.log.read_text().splitlines())
        record = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual("FAIL", record["verdict"])
        self.assertIn("source-call", record["reason"])

    def test_rejects_incoherent_abi_and_source_call_totals(self):
        self._phase(
            "surface-verification",
            surface={
                "schema": 1,
                "verdict": "PASS",
                "options": 130,
                "exports": {"total": 603, "source_call": 52, "abi_export": 550},
            },
        )
        result = self._run()
        self.assertNotEqual(0, result.returncode)
        record = json.loads(self.output.read_text(encoding="utf-8"))
        self.assertEqual("FAIL", record["verdict"])
        self.assertIn("incoherent", record["reason"])

    def test_runner_is_packaged_with_its_regression_test(self):
        root_makefile = (ROOT / "Makefile.am").read_text(encoding="utf-8")
        tests_makefile = (ROOT / "tests" / "Makefile.am").read_text(encoding="utf-8")
        self.assertIn("scripts/run-complete-qualification.sh", root_makefile)
        self.assertIn("python/test_complete_qualification.py", tests_makefile)

    def test_report_template_has_identity_operation_and_final_state_fields(self):
        template = (
            ROOT / "docs" / "qualification" / "complete-ltfs-report-template.md"
        ).read_text(encoding="utf-8")
        for required in (
            "Commit SHA",
            "Tree SHA",
            "RPM SHA-256",
            "DB physical label",
            "MAM barcode",
            "LTFS index label",
            "DB tape serial",
            "MAM tape serial",
            "Volume UUID",
            "Index generation",
            "Final media state",
            "Artifact SHA-256",
            "Log SHA-256",
        ):
            with self.subTest(required=required):
                self.assertIn(required, template)


if __name__ == "__main__":
    unittest.main()
