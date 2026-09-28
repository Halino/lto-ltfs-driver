# SPDX-License-Identifier: BSD-3-Clause

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from tests.python.test_event_schema import SchemaViolation, fallback_validate, validate

ROOT = Path(__file__).resolve().parents[2]
INFO_BINARY = Path(
    os.environ.get("LTFS_INFO_BINARY", ROOT / "src" / "utils" / "ltfs-info")
)
INFO_CLI_BINARY = Path(
    os.environ.get("LTFS_INFO_CLI_BINARY", ROOT / "src" / "utils" / "ltfs-info")
)
INFO_SCHEMA = ROOT / "schemas" / "ltfs-info-v2.schema.json"

READY_RECORD = {
    "schema": 2,
    "media_state": "ltfs",
    "tape_by_id": "/dev/tape/by-id/test-drive-nst",
    "scsi_by_id": "/dev/lto-archiver-scsi-test-drive-sg",
    "drive_serial": "DRIVE-TEST-01",
    "mam_barcode": "TEST01",
    "mam_volume_serial": "SERIAL-TEST-01",
    "ltfs_volume_label": "TEST VOLUME",
    "ltfs_volume_uuid": "11111111-2222-3333-4444-555555555555",
    "index_generation": 42,
}
UNIDENTIFIED_RECORD = {
    "schema": 2,
    "media_state": "unidentified",
    "tape_by_id": "/dev/tape/by-id/test-drive-nst",
    "scsi_by_id": "/dev/lto-archiver-scsi-test-drive-sg",
    "drive_serial": "DRIVE-TEST-01",
    "mam_barcode": "TEST01",
    "mam_volume_serial": "SERIAL-TEST-01",
    "ltfs_volume_label": None,
    "ltfs_volume_uuid": None,
    "index_generation": None,
}
UNIDENTIFIED_LTFS_WITHOUT_BARCODE_RECORD = {
    **UNIDENTIFIED_RECORD,
    "mam_barcode": None,
}


class LtfsInfoCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = json.loads(INFO_SCHEMA.read_text(encoding="utf-8"))

    def run_fixture(self, name):
        return subprocess.run(
            [str(INFO_BINARY), "--fixture", name],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_ready_json_contract(self):
        result = self.run_fixture("ready")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload, READY_RECORD)

    def test_exit_classes(self):
        for fixture, expected in (
            ("no-media", 3),
            ("identity-mismatch", 4),
            ("unsupported", 5),
            ("busy", 6),
            ("read-failure", 7),
        ):
            with self.subTest(fixture=fixture):
                result = self.run_fixture(fixture)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_normal_ltfs_partial_identity_is_rejected(self):
        result = self.run_fixture("partial")
        self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_pre_format_fake_backend_returns_unidentified_media_without_ltfs_fields(self):
        result = self.run_fixture("pre-format")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload, UNIDENTIFIED_RECORD)
        self.assertNotEqual(payload["media_state"], "blank")

    def test_pre_format_fake_backend_returns_complete_existing_ltfs_record(self):
        result = self.run_fixture("pre-format-ltfs")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload, READY_RECORD)

    def test_pre_format_ltfs_without_barcode_is_canonical_unidentified(self):
        result = self.run_fixture("pre-format-ltfs-missing-barcode")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            json.loads(result.stdout), UNIDENTIFIED_LTFS_WITHOUT_BARCODE_RECORD
        )

    def test_normal_ltfs_without_barcode_remains_strict(self):
        result = self.run_fixture("normal-ltfs-missing-barcode")
        self.assertEqual(result.returncode, 5, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_pre_format_requires_a_nonempty_valid_mam_medium_serial_number(self):
        for fixture in (
            "pre-format-missing-volume-serial",
            "pre-format-blank-medium-serial",
            "pre-format-invalid-medium-serial",
        ):
            with self.subTest(fixture=fixture):
                result = self.run_fixture(fixture)
                self.assertEqual(result.returncode, 5, result.stderr)
                self.assertEqual(result.stdout, "")

    def test_schema_accepts_complete_and_partial_records_and_is_closed(self):
        records = []
        for fixture in ("ready", "pre-format", "pre-format-ltfs-missing-barcode"):
            result = self.run_fixture(fixture)
            self.assertEqual(result.returncode, 0, result.stderr)
            records.append(json.loads(result.stdout))
        for validator in (validate, fallback_validate):
            for record in records:
                validator(record, self.schema)
        for mutation in (
            {**records[0], "extra": True},
            {key: value for key, value in records[0].items() if key != "drive_serial"},
            {**records[0], "schema": 1},
            {**records[1], "media_state": "blank"},
            {**records[0], "media_state": "unidentified"},
            {**records[1], "media_state": "ltfs"},
            {**records[0], "ltfs_volume_uuid": "not-a-uuid"},
            {**records[0], "index_generation": 0},
            {**records[1], "index_generation": -1},
            {**records[0], "mam_barcode": None},
            {**records[0], "mam_volume_serial": None},
            {**records[0], "ltfs_volume_label": None},
            {**records[0], "ltfs_volume_uuid": None},
            {**records[1], "mam_barcode": ""},
            {**records[1], "mam_barcode": 17},
            {**records[1], "mam_volume_serial": None},
            {**records[1], "ltfs_volume_label": "TEST VOLUME"},
            {
                **records[1],
                "ltfs_volume_uuid": "11111111-2222-3333-4444-555555555555",
            },
            {**records[1], "index_generation": 1},
            {
                **records[0],
                "scsi_by_id": "/dev/lto-archiver/by-id/legacy-nested-sg",
            },
        ):
            for validator in (validate, fallback_validate):
                with (
                    self.subTest(validator=validator.__name__, mutation=mutation),
                    self.assertRaises(SchemaViolation),
                ):
                    validator(mutation, self.schema)

    def test_wire_serial_name_documents_medium_serial_number_0401(self):
        self.assertEqual(
            self.schema["properties"]["mam_volume_serial"]["description"],
            "Compatibility key carrying read-only MAM Medium Serial Number 0x0401.",
        )

    def test_pre_format_file_backend_preserves_fixture_byte_exact(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary) / "blank-media.txt"
            fixture.write_bytes(b"NON-LTFS\nTEST01\nSERIAL-TEST-01\n")
            before_bytes = fixture.read_bytes()
            before_stat = fixture.stat()
            result = subprocess.run(
                [str(INFO_BINARY), "--file-fixture", str(fixture)],
                text=True,
                capture_output=True,
                check=False,
            )
            after_stat = fixture.stat()
            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertEqual(payload, UNIDENTIFIED_RECORD)
            self.assertEqual(fixture.read_bytes(), before_bytes)
            self.assertEqual(after_stat.st_size, before_stat.st_size)
            self.assertEqual(after_stat.st_mode, before_stat.st_mode)
            self.assertEqual(after_stat.st_mtime_ns, before_stat.st_mtime_ns)
            self.assertEqual(after_stat.st_ctime_ns, before_stat.st_ctime_ns)

    def test_true_cli_self_test_exposes_pre_format_contract(self):
        result = subprocess.run(
            [str(INFO_CLI_BINARY), "--self-test-fixture", "pre-format"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(
            payload,
            {
                "schema": 2,
                "media_state": "unidentified",
                "tape_by_id": "/dev/tape/by-id/self-test-nst",
                "scsi_by_id": "/dev/lto-archiver-scsi-self-test-sg",
                "drive_serial": "SELFTEST",
                "mam_barcode": "SELFTEST01",
                "mam_volume_serial": "SELFTEST-SERIAL-01",
                "ltfs_volume_label": None,
                "ltfs_volume_uuid": None,
                "index_generation": None,
            },
        )

    def test_true_cli_advertises_pre_format_without_device_access(self):
        result = subprocess.run(
            [str(INFO_CLI_BINARY), "--help"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--json --mode unmounted|pre-format", result.stderr)

    def test_unknown_fixture_is_usage_error(self):
        result = self.run_fixture("unknown")
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")

    def test_capacity_self_test_is_separate_from_identity_schema(self):
        result = subprocess.run(
            [str(INFO_CLI_BINARY), "--self-test-fixture", "capacity"],
            text=True, capture_output=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["kind"], "ltfs-capacity")
        self.assertEqual(payload["schema"], 1)
        self.assertIs(payload["identity_verified_before_after"], True)
        self.assertEqual(payload["log_page"], 49)
        self.assertEqual(payload["capacity_offset"], 0)
        self.assertEqual(payload["unit"], "MiB")
        self.assertEqual(payload["remaining_partition1_mib"], 2)
        self.assertEqual(payload["maximum_partition1_mib"], 4)
        validate(payload["identity"], self.schema)

    def test_capacity_incomplete_or_malformed_expectations_fail_before_device_access(self):
        arguments = [
            "--json", "--mode", "capacity",
            "--expect-drive-serial", "TEST",
            "--expect-medium-serial", "SERIAL",
            "--expect-label", "LABEL",
            "--expect-uuid", "11111111-2222-3333-4444-555555555555",
            "--expect-generation", "42",
        ]
        cases = [arguments[:i] + arguments[i + 2:] for i in range(3, len(arguments), 2)]
        cases += [arguments[:-1] + [value] for value in ("", "0", "-1", "+1", "1x", "18446744073709551616")]
        cases += [arguments + ["--expect-label", "DUPLICATE"]]
        cases += [[*arguments[:2], "unmounted", *arguments[3:]]]
        for args in cases:
            with self.subTest(arguments=args):
                result = subprocess.run([str(INFO_CLI_BINARY), *args],
                                        text=True, capture_output=True, check=False)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
