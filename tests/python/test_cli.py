# SPDX-License-Identifier: BSD-3-Clause

import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("LTFS_BUILD_DIR", ROOT))


class CliTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)

    def test_version_needs_no_device_or_config(self):
        env = {"PATH": os.environ["PATH"], "HOME": self.tempdir.name}
        run = subprocess.run(
            [str(BUILD / "src/ltfs"), "--version"],
            env=env,
            text=True,
            capture_output=True,
            timeout=5,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "lto-ltfs 0.1.0\n")
        self.assertEqual(run.stderr, "")

    def test_version_ignores_blocking_config_and_device_inputs(self):
        config_fifo = Path(self.tempdir.name) / "config.fifo"
        device_fifo = Path(self.tempdir.name) / "device.fifo"
        os.mkfifo(config_fifo)
        os.mkfifo(device_fifo)
        env = {"PATH": os.environ["PATH"], "HOME": self.tempdir.name}
        run = subprocess.run(
            [
                str(BUILD / "src/ltfs"),
                "-o",
                f"config_file={config_fifo}",
                "-o",
                f"devname={device_fifo}",
                "--version",
            ],
            env=env,
            text=True,
            capture_output=True,
            timeout=5,
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "lto-ltfs 0.1.0\n")
        self.assertEqual(run.stderr, "")

    def test_event_and_index_options(self):
        binary = str(BUILD / "src/ltfs")
        operation_id = "00000000-0000-4000-8000-000000000001"
        invalid_cases = [
            (["--event-fd"], "options.event_fd.invalid"),
            (["--event-fd=abc"], "options.event_fd.invalid"),
            (["--event-fd=-1"], "options.event_fd.invalid"),
            (["--event-fd=0"], "options.event_fd.invalid"),
            (["--event-fd=1"], "options.event_fd.invalid"),
            (["--event-fd=2"], "options.event_fd.invalid"),
            (["--event-fd=99999"], "options.event_fd.invalid"),
            (["--event-schema=0"], "options.event_schema.unsupported"),
            (["--event-schema=2"], "options.event_schema.unsupported"),
            ([f"--operation-id={operation_id}"], "options.event.required"),
            (["--operation-id=not-a-uuid"], "options.operation_id.invalid"),
            (["--index-policy=unknown"], "options.index_policy.invalid"),
            (["--index-policy=interval", "--index-interval-bytes=1"], "options.index_interval.invalid"),
            (["--index-policy=interval", "--index-interval-seconds=1"], "options.index_interval.invalid"),
            (["--index-policy=interval", "--index-interval-bytes=0", "--index-interval-seconds=1"], "options.index_interval.invalid"),
            (["--index-policy=unmount", "--index-interval-bytes=1"], "options.index_policy.conflict"),
            (["--index-policy=unmount", "-osync_type=unmount"], "options.index_policy.conflict"),
        ]
        config_fifo = Path(self.tempdir.name) / "must-not-open.conf"
        os.mkfifo(config_fifo)
        for options, message_code in invalid_cases:
            with self.subTest(options=options):
                try:
                    run = subprocess.run(
                        [binary, *options, "-o", f"config_file={config_fifo}"],
                        text=True,
                        capture_output=True,
                        timeout=2,
                    )
                except subprocess.TimeoutExpired:
                    self.fail(f"invalid options reached config access: {options}")
                self.assertEqual(run.returncode, 2, run.stderr)
                self.assertEqual(run.stdout, "")
                self.assertEqual(run.stderr, f"{message_code}\n")

        read_fd, write_fd = os.pipe()
        unrelated_read, unrelated_write = os.pipe()
        self.addCleanup(self._safe_close_fd, read_fd)
        self.addCleanup(self._safe_close_fd, write_fd)
        self.addCleanup(self._safe_close_fd, unrelated_read)
        self.addCleanup(self._safe_close_fd, unrelated_write)
        unrelated_flags = fcntl.fcntl(unrelated_write, fcntl.F_GETFL)
        missing_config = Path(self.tempdir.name) / "absent.conf"
        run = subprocess.run(
            [
                binary,
                f"--event-fd={write_fd}",
                "--event-schema=1",
                f"--operation-id={operation_id}",
                "--index-policy=unmount",
                "-o",
                f"config_file={missing_config}",
            ],
            pass_fds=(write_fd, unrelated_write),
            text=True,
            capture_output=True,
            timeout=5,
        )
        self.assertNotEqual(run.returncode, 0)
        self.assertNotIn("Invalid option", run.stderr)
        self.assertTrue(fcntl.fcntl(write_fd, fcntl.F_GETFL) & os.O_NONBLOCK)
        self.assertEqual(fcntl.fcntl(unrelated_write, fcntl.F_GETFL), unrelated_flags)
        os.close(write_fd)
        with os.fdopen(read_fd, encoding="utf-8") as reader:
            events = [json.loads(line) for line in reader]
        self.assertEqual([event["phase"] for event in events], ["STARTING", "FAILED"])
        self.assertEqual(events[0]["message_code"], "process.starting")
        self.assertEqual(events[0]["result"], 0)
        self.assertEqual(events[1]["message_code"], "process.failed")
        self.assertNotEqual(events[1]["result"], 0)

    def test_help_reports_unmount_default(self):
        config = Path(self.tempdir.name) / "help.conf"
        config.write_text(
            "plugin tape file /no/such/libtape-file.so\n"
            "default tape file\n"
            "option single-drive noallow_other\n",
            encoding="utf-8",
        )
        run = subprocess.run(
            [str(BUILD / "src/ltfs"), "-o", f"config_file={config}", "--help"],
            text=True,
            capture_output=True,
            timeout=5,
        )
        help_output = run.stdout + run.stderr
        self.assertEqual(run.returncode, 0, help_output)
        self.assertIn("Specify legacy sync type (default policy: unmount)", help_output)
        self.assertNotIn("Specify sync type (default: time@5)", help_output)
        self.assertNotIn("(default: min=5)", help_output)

    @staticmethod
    def _safe_close_fd(fd):
        try:
            os.close(fd)
        except OSError:
            pass


if __name__ == "__main__":
    unittest.main()
