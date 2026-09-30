# SPDX-License-Identifier: BSD-3-Clause
"""The permission-denied fixture must remain meaningful in root builds."""

import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "tests/c/run-device-identity.sh"


class DeviceIdentityRunnerTests(unittest.TestCase):
    def run_child(self, source):
        return subprocess.run(
            ["/bin/sh", str(RUNNER), "-c", source],
            env={**os.environ, "LTFS_DEVICE_IDENTITY_BINARY": sys.executable},
            capture_output=True, text=True, timeout=20, check=False,
        )

    def test_mode_zero_really_denies_read_access(self):
        child = self.run_child("""
import errno, os, tempfile
with tempfile.TemporaryDirectory() as directory:
    path = os.path.join(directory, 'permission-fixture')
    with open(path, 'w') as stream:
        stream.write('fixture')
    os.chmod(path, 0)
    try:
        try:
            with open(path) as stream:
                stream.read()
        except OSError as error:
            assert error.errno == errno.EACCES, error
        else:
            raise AssertionError('root bypassed mode-zero permissions')
    finally:
        os.chmod(path, 0o600)
""")
        self.assertEqual(child.returncode, 0, child.stderr)

    def test_native_failure_is_not_hidden(self):
        child = self.run_child("raise SystemExit(37)")
        self.assertEqual(child.returncode, 37, child.stderr)


if __name__ == "__main__":
    unittest.main()
