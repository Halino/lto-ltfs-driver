# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StreamingTelemetryBuildTests(unittest.TestCase):
    def test_production_uses_writer_queue_not_scsi_read_ili(self):
        sg = (ROOT / "src/tape_drivers/linux/sg/sg_tape.c").read_text()
        fuse = (ROOT / "src/ltfs_fuse.c").read_text()
        unified = (ROOT / "src/iosched/unified.c").read_text()
        self.assertNotIn("buffer_underrun_count", sg)
        self.assertIn("iosched_get_buffer_underrun_count", fuse)
        self.assertIn("ltfs_streaming_stats_write", unified)
        self.assertIn("ltfs_streaming_stats_wait", unified)
        self.assertIn("priv->ws_count > 0", unified)

    def test_failure_sensitive_state_machine_is_in_test_gate(self):
        makefile = (ROOT / "tests/Makefile.am").read_text().replace("\\\n", " ")
        self.assertIn("test_streaming_stats", makefile)
        self.assertIn("python/test_streaming_telemetry_build.py", makefile)


if __name__ == "__main__": unittest.main()
