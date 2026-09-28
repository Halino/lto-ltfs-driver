#!/usr/bin/env python3
"""Strictly validate and compare hardware-free LTFS benchmark receipts."""

import argparse
import base64
import hashlib
import json
import re
from pathlib import Path

SHA256 = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
UUID = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
SCHEMA = json.loads((Path(__file__).with_name("schema-v1.json")).read_text())


def _keys(value, expected, name):
    if type(value) is not dict or set(value) != set(expected):
        raise ValueError(f"{name} has unexpected schema")


def _uint(value, name, positive=False):
    if type(value) is not int or value < (1 if positive else 0):
        raise ValueError(f"{name} is invalid")


def validate_result(data):
    try:
        import jsonschema
    except ImportError:
        pass
    else:
        try:
            jsonschema.Draft202012Validator(SCHEMA).validate(data)
        except jsonschema.ValidationError as error:
            raise ValueError("benchmark JSON schema violation") from error
    _keys(data, ("schema", "build", "environment", "backend", "settings",
                 "workloads", "metrics", "validation"), "result")
    if data["schema"] != 1: raise ValueError("unknown schema")
    build = data["build"]
    _keys(build, ("project", "git_commit", "dirty", "python", "artifacts"), "build")
    if build["project"] != "lto-ltfs" or not COMMIT.fullmatch(build["git_commit"]):
        raise ValueError("untrusted build identity")
    if build["dirty"] is not False: raise ValueError("dirty builds are rejected")
    artifacts = build["artifacts"]
    _keys(artifacts, ("ltfs", "mkltfs", "tape_backend", "iosched_backend",
                      "config"), "artifacts")
    if any(not SHA256.fullmatch(value or "") for value in artifacts.values()):
        raise ValueError("build artifact digest invalid")
    env = data["environment"]
    _keys(env, ("system", "release", "machine", "cpu_count", "cpu_model",
                "ram_bytes", "source_filesystem_device", "hba", "driver",
                "firmware", "tape_alias"), "environment")
    if any(type(env[k]) is not str for k in ("system", "release", "machine")):
        raise ValueError("environment strings invalid")
    if env["cpu_count"] is not None: _uint(env["cpu_count"], "cpu count", True)
    if env["ram_bytes"] is not None: _uint(env["ram_bytes"], "RAM", True)
    if env["cpu_model"] is not None and type(env["cpu_model"]) is not str:
        raise ValueError("CPU model invalid")
    _uint(env["source_filesystem_device"], "filesystem device")
    if any(env[k] is not None for k in ("hba", "driver", "firmware", "tape_alias")):
        raise ValueError("file benchmark cannot claim physical hardware")
    backend = data["backend"]
    _keys(backend, ("kind", "implementation", "physical_tape", "tape_backend_path"), "backend")
    if backend["kind"] != "file" or backend["implementation"] != "ltfs-libtape-file" or backend["physical_tape"] is not False:
        raise ValueError("only the real LTFS file backend is comparable")
    if type(backend["tape_backend_path"]) is not str or not backend["tape_backend_path"]:
        raise ValueError("backend path unavailable")
    settings = data["settings"]
    _keys(settings, ("seed", "large_bytes", "copy_buffer_bytes", "compression_enabled",
                     "sync_type", "mount_readback_readonly"), "settings")
    for key in ("seed", "large_bytes", "copy_buffer_bytes"): _uint(settings[key], key)
    if settings["compression_enabled"] is not False or settings["sync_type"] != "unmount" or settings["mount_readback_readonly"] is not True:
        raise ValueError("benchmark settings invalid")
    workloads = data["workloads"]
    if type(workloads) is not list or not workloads: raise ValueError("workloads unavailable")
    expected = {}
    for item in workloads:
        _keys(item, ("name", "size_bytes", "sha256"), "workload")
        if type(item["name"]) is not str or item["name"] in expected or not SHA256.fullmatch(item["sha256"]):
            raise ValueError("workload identity invalid")
        _uint(item["size_bytes"], "workload size")
        expected[item["name"]] = item["sha256"]
    metrics = data["metrics"]
    _keys(metrics, ("duration_ns", "total_bytes", "file_count",
                    "throughput_bytes_per_second", "retry_count", "error_count",
                    "finalization_count"), "metrics")
    for key in metrics: _uint(metrics[key], key, key in ("duration_ns", "throughput_bytes_per_second"))
    if metrics["total_bytes"] != sum(i["size_bytes"] for i in workloads) or metrics["file_count"] != len(workloads) or metrics["error_count"] != 0 or metrics["finalization_count"] != 2:
        raise ValueError("metrics contradict workload")
    if metrics["throughput_bytes_per_second"] != (
            metrics["total_bytes"] * 1_000_000_000 // metrics["duration_ns"]):
        raise ValueError("throughput contradicts duration and byte count")
    validation = data["validation"]
    _keys(validation, ("method", "all_hashes_match", "files", "receipts"), "validation")
    if validation["method"] != "ltfs-unmount-remount-index-hash" or validation["all_hashes_match"] is not True:
        raise ValueError("remount validation missing")
    files = validation["files"]
    if type(files) is not list or len(files) != len(expected): raise ValueError("validation incomplete")
    seen = set()
    for item in files:
        _keys(item, ("name", "expected_sha256", "observed_sha256", "matches"), "hash receipt")
        name = item["name"]
        if name in seen or name not in expected or item["expected_sha256"] != expected[name] or not SHA256.fullmatch(item["observed_sha256"] or "") or item["observed_sha256"] != expected[name] or item["matches"] is not True:
            raise ValueError("hash receipt is forged or incomplete")
        seen.add(name)
    if seen != set(expected): raise ValueError("hash receipt missing")
    receipts = validation["receipts"]
    if type(receipts) is not list or len(receipts) != 2:
        raise ValueError("finalization receipts missing")
    receipt_keys = {"schema", "stage", "operation_id", "volume_uuid", "prior_generation", "new_generation",
        "bytes_valid", "bytes", "files_valid", "files", "phase_duration_ns",
        "capture_duration_ns", "device_close_duration_ns", "device_close_result_valid",
        "device_close_result", "catalog_ack_duration_ns", "media_committed",
        "catalog_acknowledged", "cleanup_failed", "result"}
    for index, item in enumerate(receipts):
        _keys(item, ("purpose", "sha256", "content_base64"), "finalization receipt")
        if item["purpose"] != ("write" if index == 0 else "readback") or not SHA256.fullmatch(item["sha256"] or ""):
            raise ValueError("finalization receipt identity invalid")
        try: raw = base64.b64decode(item["content_base64"], validate=True); receipt = json.loads(raw)
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError("finalization receipt payload invalid") from error
        if hashlib.sha256(raw).hexdigest() != item["sha256"] or type(receipt) is not dict or set(receipt) != receipt_keys:
            raise ValueError("finalization receipt digest/schema invalid")
        expected_bytes = metrics["total_bytes"] if index == 0 else 0
        expected_files = metrics["file_count"] if index == 0 else 0
        uint_fields = ("prior_generation", "new_generation", "bytes", "files",
                       "capture_duration_ns", "device_close_duration_ns",
                       "catalog_ack_duration_ns")
        bool_fields = ("bytes_valid", "files_valid", "device_close_result_valid",
                       "media_committed", "catalog_acknowledged", "cleanup_failed")
        if (receipt["schema"] != 1 or receipt["stage"] != "terminal" or
                type(receipt["operation_id"]) is not str or not UUID.fullmatch(receipt["operation_id"]) or
                type(receipt["volume_uuid"]) is not str or not UUID.fullmatch(receipt["volume_uuid"]) or
                any(type(receipt[name]) is not int or receipt[name] < 0 for name in uint_fields) or
                any(type(receipt[name]) is not bool for name in bool_fields) or
                type(receipt["device_close_result"]) is not int or type(receipt["result"]) is not int or
                type(receipt["new_generation"]) is not int or receipt["new_generation"] <= 0 or
                receipt["new_generation"] < receipt["prior_generation"] or
                receipt["bytes_valid"] is not True or receipt["bytes"] != expected_bytes or
                receipt["files_valid"] is not True or receipt["files"] != expected_files or
                (index == 1 and receipt["new_generation"] != receipt["prior_generation"]) or
                receipt["media_committed"] is not True or receipt["catalog_acknowledged"] is not True or
                receipt["device_close_result_valid"] is not True or receipt["device_close_result"] != 0 or
                receipt["cleanup_failed"] is not False or receipt["result"] != 0 or
                type(receipt["phase_duration_ns"]) is not list or len(receipt["phase_duration_ns"]) != 11 or
                any(type(value) is not int or value < 0 for value in receipt["phase_duration_ns"])):
            raise ValueError("finalization receipt contradicts metrics")
    decoded = [json.loads(base64.b64decode(item["content_base64"], validate=True))
               for item in receipts]
    if decoded[0]["volume_uuid"] != decoded[1]["volume_uuid"]:
        raise ValueError("remount receipt volume identity changed")
    return data


def _load(path):
    return validate_result(json.loads(path.read_text()))


def more_than_five_percent_slower(baseline_rate, candidate_rate):
    return candidate_rate * 100 < baseline_rate * 95


def regressed(baseline, candidate):
    validate_result(baseline); validate_result(candidate)
    for key in ("environment", "backend", "settings", "workloads"):
        if baseline[key] != candidate[key]: raise ValueError("results are not comparable")
    return more_than_five_percent_slower(
        baseline["metrics"]["throughput_bytes_per_second"],
        candidate["metrics"]["throughput_bytes_per_second"])


def main():
    parser = argparse.ArgumentParser(); parser.add_argument("baseline", type=Path); parser.add_argument("candidate", type=Path)
    args = parser.parse_args()
    try: return 1 if regressed(_load(args.baseline), _load(args.candidate)) else 0
    except (OSError, ValueError, json.JSONDecodeError): return 2


if __name__ == "__main__": raise SystemExit(main())
