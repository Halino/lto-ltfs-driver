# SPDX-License-Identifier: BSD-3-Clause

import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "qualification" / "run-file-destructive.py"


def load_runner():
    spec = importlib.util.spec_from_file_location("run_file_destructive", RUNNER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeProcess:
    def __init__(self, commands, receipt, operation_id, readonly):
        self.commands = commands
        self.receipt = receipt
        self.operation_id = operation_id
        self.readonly = readonly
        self.finished = False

    def poll(self):
        return 0 if self.finished else None

    def wait(self, timeout=None):
        del timeout
        self.finished = True
        files = 0 if self.readonly else len(self.commands.media)
        total = 0 if self.readonly else sum(map(len, self.commands.media.values()))
        receipt = {
            "schema": 1,
            "stage": "terminal",
            "operation_id": self.operation_id,
            "volume_uuid": "22222222-2222-4222-8222-222222222222",
            "prior_generation": self.commands.generation,
            "new_generation": self.commands.generation,
            "bytes_valid": True,
            "bytes": total,
            "files_valid": True,
            "files": files,
            "phase_duration_ns": [0] * 11,
            "capture_duration_ns": 0,
            "device_close_duration_ns": 0,
            "device_close_result_valid": True,
            "device_close_result": 0,
            "catalog_ack_duration_ns": 0,
            "media_committed": True,
            "catalog_acknowledged": True,
            "cleanup_failed": False,
            "result": 0,
        }
        self.receipt.write_text(
            json.dumps(receipt, separators=(",", ":")) + "\n", encoding="utf-8"
        )
        self.receipt.chmod(0o600)
        return 0


class FakeCommands:
    def __init__(self):
        self.argv = []
        self.media = {}
        self.label = None
        self.generation = 1
        self.mounted = False
        self.mountpoint = None
        self.readonly = False
        self.process = None

    def run(self, argv, **kwargs):
        del kwargs
        argv = list(map(str, argv))
        self.argv.append(argv)
        name = Path(argv[0]).name
        if name == "findmnt":
            return subprocess.CompletedProcess(argv, 0 if self.mounted else 1, "", "")
        if name == "mkltfs":
            self.media.clear()
            self.label = argv[argv.index("--volume-name") + 1]
            self.generation = 1
            return subprocess.CompletedProcess(
                argv, 1 if "--wipe" in argv or "--long-wipe" in argv else 0, "", ""
            )
        if name == "ltfsck":
            return subprocess.CompletedProcess(argv, 0, "", "")
        if name == "fusermount":
            if not self.readonly:
                self.media = {
                    item.name: item.read_bytes()
                    for item in self.mountpoint.iterdir()
                    if item.is_file()
                }
                self.generation += 1
            for item in tuple(self.mountpoint.iterdir()):
                if item.is_file():
                    item.unlink()
            self.mounted = False
            return subprocess.CompletedProcess(argv, 0, "", "")
        raise AssertionError(argv)

    def start(self, argv, **kwargs):
        del kwargs
        argv = list(map(str, argv))
        self.argv.append(argv)
        self.mountpoint = Path(argv[2])
        options = argv[argv.index("-o") + 1].split(",")
        self.readonly = "ro" in options
        receipt = Path(
            next(
                value.split("=", 1)[1]
                for value in options
                if value.startswith("standalone_receipt=")
            )
        )
        operation_id = argv[1].split("=", 1)[1]
        for name, payload in self.media.items():
            (self.mountpoint / name).write_bytes(payload)
        self.mounted = True
        self.process = FakeProcess(self, receipt, operation_id, self.readonly)
        return self.process


class FileDestructiveQualificationTests(unittest.TestCase):
    def test_default_is_plan_only_and_executes_no_command(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "plan.json"
            result = subprocess.run(
                [sys.executable, os.fspath(RUNNER), "--output", os.fspath(output)],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            plan = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(1, plan["schema"])
        self.assertEqual("PLAN", plan["verdict"])
        self.assertEqual("file", plan["backend"])
        self.assertIs(plan["physical"], False)
        self.assertEqual(
            [
                "format",
                "mount-rw",
                "create-write-append-rename-truncate-delete",
                "unmount-commit",
                "ltfsck",
                "mount-ro-readback",
                "short-wipe",
                "long-wipe",
                "destructive-reformat",
                "mount-ro-empty-verification",
            ],
            plan["operations"],
        )

    def test_execute_rejects_a_character_device_before_any_command(self):
        runner = load_runner()

        class NoCommands:
            def run(self, argv, **kwargs):
                raise AssertionError(argv)

            def start(self, argv, **kwargs):
                raise AssertionError(argv)

        with tempfile.TemporaryDirectory() as temporary:
            owned = Path(temporary)
            owned.chmod(0o700)
            with self.assertRaisesRegex(
                runner.QualificationRefused, "virtual cartridge"
            ):
                runner.run(
                    owned,
                    Path("/dev/null"),
                    "SYNTHETIC/TEST\\LABEL",
                    None,
                    commands=NoCommands(),
                )

    def test_execute_cli_accepts_only_the_complete_explicit_toolchain(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "evidence.json"
            cartridge = root / "cartridge"
            cartridge.mkdir(mode=0o700)
            missing = root / "missing-tool"
            argv = [
                sys.executable,
                os.fspath(RUNNER),
                "--output",
                os.fspath(output),
                "--execute-file-backend",
                "--owned-root",
                os.fspath(root),
                "--cartridge-directory",
                os.fspath(cartridge),
            ]
            for option in (
                "ltfs",
                "mkltfs",
                "ltfsck",
                "tape-backend",
                "iosched-backend",
                "config",
                "fusermount",
                "findmnt",
            ):
                argv.extend((f"--{option}", os.fspath(missing)))
            result = subprocess.run(argv, text=True, capture_output=True, check=False)
        self.assertEqual(2, result.returncode)
        self.assertIn("file-backend toolchain is invalid", result.stderr)
        self.assertNotIn("unrecognized arguments", result.stderr)

    def test_label_is_data_and_never_becomes_a_path_component(self):
        runner = load_runner()
        label = "SYNTHETIC_TEST_LABEL"
        plan = runner.plan(label)
        self.assertEqual(label, plan["physical_label"])
        self.assertNotIn(label, plan["owned_names"])
        self.assertEqual(
            {"cartridge", "mount", "receipts", "evidence.json"},
            set(plan["owned_names"]),
        )

    def test_complete_file_backend_workflow_is_destructive_and_hash_verified(self):
        runner = load_runner()
        commands = FakeCommands()
        label = "SYNTHETIC/TEST\\LABEL"
        with tempfile.TemporaryDirectory() as temporary:
            owned = Path(temporary)
            owned.chmod(0o700)
            cartridge = owned / "cartridge"
            cartridge.mkdir(mode=0o700)
            tools = runner.Toolchain(
                *(
                    owned / name
                    for name in (
                        "ltfs",
                        "mkltfs",
                        "ltfsck",
                        "libtape-file.so",
                        "libiosched-fcfs.so",
                        "ltfs.conf",
                        "fusermount",
                        "findmnt",
                    )
                )
            )
            for path in tools:
                path.touch(mode=0o700)
            evidence = runner.run(owned, cartridge, label, tools, commands=commands)
        self.assertEqual("PASS", evidence["verdict"])
        self.assertEqual(label, evidence["physical_label"])
        self.assertIs(evidence["physical"], False)
        self.assertEqual(evidence["written_sha256"], evidence["readback_sha256"])
        self.assertEqual(
            hashlib.sha256(b"alpha\nb").hexdigest(), evidence["written_sha256"]
        )
        mkltfs = [argv for argv in commands.argv if Path(argv[0]).name == "mkltfs"]
        self.assertEqual(4, len(mkltfs))
        self.assertTrue(
            all(argv[argv.index("--tape-serial") + 1] == "SYN001" for argv in mkltfs)
        )
        mounts = [argv for argv in commands.argv if Path(argv[0]).name == "ltfs"]
        self.assertEqual(3, len(mounts))
        for argv in mounts:
            self.assertRegex(argv[1], r"^--operation-id=[0-9a-f-]{36}$")
            self.assertNotIn("operation_id=", argv[argv.index("-o") + 1])
        self.assertFalse(any(label in part for part in evidence["owned_paths"]))
        self.assertEqual(
            [None, "--wipe", "--long-wipe", "--destructive"],
            [
                next(
                    (
                        flag
                        for flag in ("--wipe", "--long-wipe", "--destructive")
                        if flag in argv
                    ),
                    None,
                )
                for argv in mkltfs
            ],
        )
        self.assertEqual({}, commands.media)


if __name__ == "__main__":
    unittest.main()
