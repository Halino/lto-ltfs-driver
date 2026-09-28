# SPDX-License-Identifier: BSD-3-Clause

import copy
import json
import os
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCHEMA_PATH = ROOT / "schemas/ltfs-event-v1.schema.json"
EVENT_BINARY = Path(
    os.environ.get("LTFS_EVENT_FIXTURE_BINARY", ROOT / "tests/test_events")
)


class SchemaViolation(ValueError):
    pass


def fallback_validate(instance, schema, path="$"):
    expected_type = schema.get("type")
    if expected_type is not None:
        expected_types = (
            expected_type if isinstance(expected_type, list) else [expected_type]
        )
        type_checks = {
            "object": lambda value: isinstance(value, dict),
            "string": lambda value: isinstance(value, str),
            "integer": lambda value: isinstance(value, int)
            and not isinstance(value, bool),
			"boolean": lambda value: isinstance(value, bool),
            "null": lambda value: value is None,
        }
        if not any(type_checks[kind](instance) for kind in expected_types):
            raise SchemaViolation(f"{path}: wrong type")
    if "const" in schema and instance != schema["const"]:
        raise SchemaViolation(f"{path}: wrong constant")
    if "enum" in schema and instance not in schema["enum"]:
        raise SchemaViolation(f"{path}: value is not in enum")
    if "oneOf" in schema:
        matches = 0
        for option in schema["oneOf"]:
            try:
                fallback_validate(instance, option, path)
            except SchemaViolation:
                continue
            matches += 1
        if matches != 1:
            raise SchemaViolation(f"{path}: expected exactly one matching schema")
    if isinstance(instance, int) and not isinstance(instance, bool):
        if "minimum" in schema and instance < schema["minimum"]:
            raise SchemaViolation(f"{path}: below minimum")
        if "maximum" in schema and instance > schema["maximum"]:
            raise SchemaViolation(f"{path}: above maximum")
    if isinstance(instance, str):
        if "maxLength" in schema and len(instance) > schema["maxLength"]:
            raise SchemaViolation(f"{path}: string too long")
        if "pattern" in schema and re.fullmatch(schema["pattern"], instance) is None:
            raise SchemaViolation(f"{path}: pattern mismatch")
    if isinstance(instance, dict):
        missing = set(schema.get("required", [])) - set(instance)
        if missing:
            raise SchemaViolation(f"{path}: missing {sorted(missing)}")
        properties = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            extra = set(instance) - set(properties)
            if extra:
                raise SchemaViolation(f"{path}: extra {sorted(extra)}")
        for key, value in instance.items():
            if key in properties:
                fallback_validate(value, properties[key], f"{path}.{key}")


def validate(instance, schema):
    try:
        import jsonschema
    except ImportError:
        fallback_validate(instance, schema)
    else:
        jsonschema.Draft202012Validator.check_schema(schema)
        try:
            jsonschema.validate(instance=instance, schema=schema)
        except jsonschema.ValidationError as error:
            raise SchemaViolation(str(error)) from error


class EventSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        result = subprocess.run(
            [str(EVENT_BINARY), "--emit-fixtures"],
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        if result.stderr != "":
            raise AssertionError(f"unexpected fixture stderr: {result.stderr!r}")
        cls.fixtures = [json.loads(line) for line in result.stdout.splitlines()]

    def test_every_emitted_fixture_validates(self):
        self.assertEqual(len(self.fixtures), 2)
        for fixture in self.fixtures:
            validate(fixture, self.schema)
        self.assertEqual([event["seq"] for event in self.fixtures], [1, 2])
        self.assertEqual(self.fixtures[0]["device_serial"], 'synthetic"drive\n01')
        self.assertIn("result", self.fixtures[0])
        self.assertEqual(self.fixtures[0]["result"], 0)

    def test_required_machine_fields_cannot_be_omitted(self):
        for field in (
            "schema",
            "operation_id",
			"volume_uuid",
			"prior_generation",
			"new_generation",
            "seq",
            "phase",
            "status",
            "message_code",
            "result",
            "device_close_result",
			"queue_fill_bytes",
			"buffer_underrun_count",
			"retry_count",
			"memory_high_water_bytes",
			"telemetry_overflowed",
            "phase_elapsed_ns",
            "index_done",
            "index_total",
        ):
            with self.subTest(field=field):
                fixture = copy.deepcopy(self.fixtures[0])
                del fixture[field]
                with self.assertRaises(SchemaViolation):
                    validate(fixture, self.schema)

    def test_unknown_schema_version_is_rejected(self):
        fixture = copy.deepcopy(self.fixtures[0])
        fixture["schema"] = 2
        with self.assertRaises(SchemaViolation):
            validate(fixture, self.schema)

    def test_schema_is_closed_to_sensitive_free_text(self):
        fixture = copy.deepcopy(self.fixtures[0])
        fixture["message"] = "/private/source/path secret=value"
        with self.assertRaises(SchemaViolation):
            validate(fixture, self.schema)


if __name__ == "__main__":
    unittest.main()
