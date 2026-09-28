# SPDX-License-Identifier: BSD-3-Clause

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("LTFS_BUILD_DIR", ROOT))
LTFS = Path(os.environ.get("LTFS_BINARY", BUILD / "src" / "ltfs"))
MKLTFS = Path(os.environ.get("MKLTFS_BINARY", BUILD / "src" / "utils" / "mkltfs"))
LTFSCK = Path(os.environ.get("LTFSCK_BINARY", BUILD / "src" / "utils" / "ltfsck"))
LTFS_INFO = Path(
    os.environ.get("LTFS_INFO_CLI_BINARY", BUILD / "src" / "utils" / "ltfs-info")
)
LONG_OPTION = re.compile(r"--[a-z0-9][a-z0-9-]*(?:=[^\s,;]+)?")
FUSE_OPTION = re.compile(r"(?:^|\s)-o\s+([a-z][a-z0-9_]*)(?:=[^\s,;]+)?")
KMI_OPTIONS = {
    "-o:kmi_dk",
    "-o:kmi_dk_for_format",
    "-o:kmi_dk_list",
    "-o:kmi_dki",
    "-o:kmi_dki_for_format",
}


def options_from(output: str) -> set[str]:
    options = {match.split("=", 1)[0] for match in LONG_OPTION.findall(output)}
    options.update(f"-o:{match}" for match in FUSE_OPTION.findall(output))
    return options


def command_environment(home: str) -> dict[str, str]:
    environment = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "HOME": home}
    library_path = os.environ.get("LD_LIBRARY_PATH")
    if library_path:
        environment["LD_LIBRARY_PATH"] = library_path
    return environment


@unittest.skipUnless(
    all(
        path.is_file() and os.access(path, os.X_OK)
        for path in (LTFS, MKLTFS, LTFSCK, LTFS_INFO)
    ),
    "configured LTFS binaries are unavailable",
)
class CliSurfaceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.config = Path(self.temporary.name) / "ltfs.conf"
        self.config.write_text(
            "plugin tape file /no/such/libtape-file.so\n"
            "default tape file\n"
            "option single-drive noallow_other\n",
            encoding="utf-8",
        )

    def _help(self, binary: Path, *argv: str, exits=(0,)) -> set[str]:
        result = subprocess.run(
            [str(binary), *argv],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
            env=command_environment(self.temporary.name),
        )
        self.assertIn(result.returncode, exits, result.stderr)
        return options_from(result.stdout + result.stderr)

    def _assert_core_and_optional_kmi(self, expected, actual):
        advertised_kmi = actual & KMI_OPTIONS
        self.assertIn(advertised_kmi, (set(), KMI_OPTIONS))
        self.assertEqual(expected, actual - KMI_OPTIONS)

    def test_mkltfs_advanced_help_is_closed(self):
        self._assert_core_and_optional_kmi(
            {
                "--advanced-help",
                "--backend",
                "--blocksize",
                "--config",
                "--destructive",
                "--device",
                "--force",
                "--fulltrace",
                "--help",
                "--keep-capacity",
                "--kmi-backend",
                "--long-wipe",
                "--no-compression",
                "--no-override",
                "--quiet",
                "--rules",
                "--syslogtrace",
                "--tape-serial",
                "--trace",
                "--version",
                "--volume-name",
                "--wipe",
            },
            self._help(MKLTFS, "--config", str(self.config), "--advanced-help"),
        )

    def test_ltfsck_advanced_help_is_closed(self):
        self._assert_core_and_optional_kmi(
            {
                "--advanced-help",
                "--backend",
                "--capture-index",
                "--config",
                "--deep-recovery",
                "--erase-history",
                "--full-index-info",
                "--full-recovery",
                "--fulltrace",
                "--generation",
                "--help",
                "--keep-history",
                "--kmi-backend",
                "--list-rollback-points",
                "--no-rollback",
                "--quiet",
                "--rollback",
                "--salvage-rollback-points",
                "--syslogtrace",
                "--trace",
                "--traverse",
                "--version",
            },
            self._help(LTFSCK, "--config", str(self.config), "--advanced-help"),
        )

    def test_ltfs_info_usage_surface_is_closed(self):
        self._assert_core_and_optional_kmi(
            {
                "--device",
                "--diagnose-identity",
                "--discover",
                "--expect-drive-serial",
                "--expect-medium-serial",
                "--expect-label",
                "--expect-uuid",
                "--expect-generation",
                "--json",
                "--mode",
                "--self-test-fixture",
                "--tape-device",
                "--version",
            },
            self._help(LTFS_INFO, "--help", exits=(2,)),
        )

    def test_ltfs_mount_advanced_help_is_closed(self):
        options = self._help(
            LTFS,
            "-o",
            f"config_file={self.config}",
            "-a",
            exits=(1,),
        )
        self.assertEqual(
            {
                "--event-fd",
                "--event-schema",
                "--help",
                "--index-interval-bytes",
                "--index-interval-seconds",
                "--index-policy",
                "--operation-id",
                "--version",
                "-o:ac_attr_timeout",
                "-o:allow_other",
                "-o:allow_root",
                "-o:async_read",
                "-o:atime",
                "-o:atomic_o_trunc",
                "-o:attr_timeout",
                "-o:auto_unmount",
                "-o:big_writes",
                "-o:capture_index",
                "-o:config_file",
                "-o:congestion_threshold",
                "-o:debug",
                "-o:default_permissions",
                "-o:device_list",
                "-o:devname",
                "-o:direct_io",
                "-o:dmask",
                "-o:eject",
                "-o:entry_timeout",
                "-o:fmask",
                "-o:force_mount_no_eod",
                "-o:from_code",
                "-o:fsname",
                "-o:fulltrace",
                "-o:gid",
                "-o:hard_remove",
                "-o:intr",
                "-o:intr_signal",
                "-o:iosched_backend",
                "-o:kernel_cache",
                "-o:kmi_backend",
                "-o:large_read",
                "-o:max_background",
                "-o:max_pool_size",
                "-o:max_read",
                "-o:max_readahead",
                "-o:max_write",
                "-o:min_pool_size",
                "-o:modules",
                "-o:negative_timeout",
                "-o:no_remote_flock",
                "-o:no_remote_lock",
                "-o:no_remote_posix_lock",
                "-o:noatime",
                "-o:noeject",
                "-o:noforget",
                "-o:nonempty",
                "-o:nopath",
                "-o:opt",
                "-o:quiet",
                "-o:readdir_ino",
                "-o:release_device",
                "-o:remember",
                "-o:rollback_mount",
                "-o:rules",
                "-o:scsi_append_only_mode",
                "-o:standalone_receipt",
                "-o:subdir",
                "-o:subtype",
                "-o:sync_read",
                "-o:sync_type",
                "-o:syslogtrace",
                "-o:tape_backend",
                "-o:to_code",
                "-o:trace",
                "-o:uid",
                "-o:umask",
                "-o:use_ino",
                "-o:verbose",
                "-o:work_directory",
            },
            options,
        )

    def test_mkltfs_destructive_options_refuse_without_a_loadable_backend(self):
        image = Path(self.temporary.name) / "synthetic-tape"
        image.write_bytes(b"not a tape")
        for option in ("--force", "--wipe", "--long-wipe", "--destructive"):
            with self.subTest(option=option):
                result = subprocess.run(
                    [
                        str(MKLTFS),
                        "--config",
                        str(self.config),
                        "--device",
                        str(image),
                        option,
                    ],
                    text=True,
                    capture_output=True,
                    timeout=5,
                    check=False,
                    env=command_environment(self.temporary.name),
                )
                self.assertNotEqual(0, result.returncode)
                self.assertEqual(b"not a tape", image.read_bytes())

    def test_ltfsck_conflicting_destructive_modes_are_rejected(self):
        for options in (
            ("--rollback", "--no-rollback"),
            ("--erase-history", "--keep-history"),
            ("--deep-recovery", "--no-rollback"),
        ):
            with self.subTest(options=options):
                result = subprocess.run(
                    [
                        str(LTFSCK),
                        "--config",
                        str(self.config),
                        *options,
                        "fixture",
                    ],
                    text=True,
                    capture_output=True,
                    timeout=5,
                    check=False,
                    env=command_environment(self.temporary.name),
                )
                self.assertNotEqual(0, result.returncode)


if __name__ == "__main__":
    unittest.main()
