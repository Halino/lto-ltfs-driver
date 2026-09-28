#!/usr/bin/env python3
"""Benchmark LTFS through its real file-tape backend and a remount boundary."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from collections import namedtuple
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
_previous_dont_write_bytecode = sys.dont_write_bytecode
sys.dont_write_bytecode = True
try:
    import workloads  # noqa: E402
finally:
    sys.dont_write_bytecode = _previous_dont_write_bytecode

COPY_BUFFER_BYTES = 1024 * 1024
MAX_CONFIG_BYTES = 1024 * 1024
MAX_CONFIG_FILES = 16
PRODUCTION_FUSERMOUNT = Path("/usr/bin/fusermount")
CONFIG_DELIMITERS = frozenset(b" \t\r\n")
CONFIG_PATH_DELIMITERS = frozenset(b"\r\n")
UUID = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
Toolchain = namedtuple(
    "Toolchain",
    "ltfs mkltfs tape_backend iosched_backend config fusermount findmnt",
)


class _ConfigClosurePolicyError(Exception):
    pass


class _ConfigClosureLimitError(_ConfigClosurePolicyError):
    pass


def _config_split(value, maxsplit=-1):
    fields = []
    offset = 0
    splits = 0
    length = len(value)
    while offset < length:
        while offset < length and value[offset] in CONFIG_DELIMITERS:
            offset += 1
        if offset == length:
            break
        if maxsplit >= 0 and splits == maxsplit:
            end = length
            while end > offset and value[end - 1] in CONFIG_DELIMITERS:
                end -= 1
            fields.append(value[offset:end])
            break
        end = offset
        while end < length and value[end] not in CONFIG_DELIMITERS:
            end += 1
        fields.append(value[offset:end])
        splits += 1
        offset = end
    return fields


def _config_decode(value):
    try:
        return value.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        raise _ConfigClosurePolicyError from None


def _config_content(value):
    content = value.split(b"#", 1)[0].rstrip(b" \t\r\n")
    if any(byte < 32 and byte not in CONFIG_DELIMITERS for byte in content):
        raise _ConfigClosurePolicyError
    decoded_content = _config_decode(content)
    if any(character.isspace() and character not in " \t\r"
           for character in decoded_content):
        raise _ConfigClosurePolicyError
    return content


def _config_next(value, offset, delimiters):
    length = len(value)
    while offset < length and value[offset] in delimiters:
        offset += 1
    if offset == length:
        return None, offset
    start = offset
    while offset < length and value[offset] not in delimiters:
        offset += 1
    token = value[start:offset]
    if offset < length:
        offset += 1
    return token, offset


def _config_parse_authority(value):
    content = _config_content(value)
    directive_bytes, cursor = _config_next(content, 0, CONFIG_DELIMITERS)
    if directive_bytes is None:
        return None
    directive = _config_decode(directive_bytes)
    if directive == "plugin":
        plugin_type, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        name, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        library, cursor = _config_next(content, cursor, CONFIG_PATH_DELIMITERS)
        if plugin_type is None or name is None or library is None:
            raise RuntimeError("LTFS configuration closure is invalid")
        return tuple(map(_config_decode,
                         (directive_bytes, plugin_type, name, library)))
    if directive == "default":
        plugin_type, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        name, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        extra, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        if plugin_type is None or name is None or extra is not None:
            raise RuntimeError("LTFS configuration closure is invalid")
        return tuple(map(_config_decode, (directive_bytes, plugin_type, name)))
    if directive in {"include", "include_noerror"}:
        include, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        extra, cursor = _config_next(content, cursor, CONFIG_DELIMITERS)
        if include is None or extra is not None:
            raise RuntimeError("LTFS configuration closure is invalid")
        return tuple(map(_config_decode, (directive_bytes, include)))
    return None


class Commands:
    def run(self, argv, **kwargs):
        return subprocess.run(argv, text=True, capture_output=True, **kwargs)

    def start(self, argv, **kwargs):
        return subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, **kwargs)


def _checked(result, purpose):
    if result.returncode != 0:
        raise RuntimeError(f"{purpose} failed")
    return result


def _build(root, tools, commands):
    commit = _checked(commands.run(["git", "rev-parse", "HEAD"], cwd=root),
                      "git commit").stdout.strip()
    if len(commit) != 40 or any(c not in "0123456789abcdef" for c in commit):
        raise RuntimeError("build commit is not an exact SHA-1")
    dirty = _checked(commands.run(["git", "status", "--porcelain"], cwd=root),
                     "git status").stdout != ""
    if dirty:
        raise RuntimeError("benchmark requires an exact clean commit")
    return {"project": "lto-ltfs", "git_commit": commit, "dirty": False,
            "python": platform.python_version(),
            "artifacts": {name: _hash_file(Path(getattr(tools, name)))
                          for name in ("ltfs", "mkltfs", "tape_backend",
                                       "iosched_backend", "config")}}


def _require_tools(tools):
    for name in ("ltfs", "mkltfs", "fusermount", "findmnt"):
        path = Path(getattr(tools, name))
        if not path.is_file() or not os.access(path, os.X_OK):
            raise RuntimeError(f"required executable unavailable: {name}")
    for name in ("tape_backend", "iosched_backend", "config"):
        if not Path(getattr(tools, name)).is_file():
            raise RuntimeError(f"required LTFS prerequisite unavailable: {name}")


def _validate_config_closure(config, tape_backend, iosched_backend):
    plugins = {}
    defaults = {}
    active = set()
    files_read = 0
    total_bytes = 0

    def parse(candidate, ignore_open_error=False):
        nonlocal files_read, total_bytes
        try:
            path = candidate.resolve(strict=True)
            if not path.is_file():
                raise OSError
            if path in active:
                raise _ConfigClosureLimitError
            files_read += 1
            if files_read > MAX_CONFIG_FILES:
                raise _ConfigClosureLimitError
            with path.open("rb") as stream:
                payload = stream.read(MAX_CONFIG_BYTES + 1)
        except OSError:
            if ignore_open_error:
                return
            raise RuntimeError("LTFS configuration closure is invalid") from None
        try:
            total_bytes += len(payload)
            if len(payload) > MAX_CONFIG_BYTES or total_bytes > MAX_CONFIG_BYTES:
                raise _ConfigClosureLimitError
        except OSError:
            raise RuntimeError("LTFS configuration closure is invalid") from None
        active.add(path)
        try:
            try:
                parse_payload(payload)
            except RuntimeError:
                if not ignore_open_error:
                    raise
        finally:
            active.remove(path)

    def parse_payload(payload):
        lines = payload.split(b"\n")
        for index, encoded_line in enumerate(lines):
            physical_length = len(encoded_line) + (index < len(lines) - 1)
            if physical_length >= 65535:
                raise RuntimeError("LTFS configuration closure is invalid")
            authority = _config_parse_authority(encoded_line)
            if authority:
                directive, *fields = authority
                if directive in {"include", "include_noerror"}:
                    include = Path(fields[0])
                    if not include.is_absolute():
                        raise _ConfigClosurePolicyError
                    parse(include,
                          ignore_open_error=directive == "include_noerror")
                elif directive == "plugin":
                    plugin_type, name, library_text = fields
                    library = Path(library_text)
                    if not library.is_absolute():
                        raise _ConfigClosurePolicyError
                    plugins[(plugin_type, name)] = library
                else:
                    defaults[fields[0]] = fields[1]
                continue
            content = _config_content(encoded_line)
            fields = _config_split(content, 1)
            if not fields:
                continue
            directive = _config_decode(fields[0])
            remainder = fields[1] if len(fields) == 2 else b""
            if directive == "option":
                fields = _config_split(remainder, 1)
                if len(fields) != 2:
                    raise RuntimeError("LTFS configuration closure is invalid")
            elif directive == "-default":
                fields = list(map(_config_decode, _config_split(remainder)))
                if len(fields) != 1 or fields[0] not in defaults:
                    raise RuntimeError("LTFS configuration closure is invalid")
                del defaults[fields[0]]
            elif directive == "-plugin":
                fields = list(map(_config_decode, _config_split(remainder)))
                if len(fields) != 2:
                    raise RuntimeError("LTFS configuration closure is invalid")
                plugins.pop((fields[0], fields[1]), None)

    try:
        parse(Path(config))
    except _ConfigClosurePolicyError:
        raise RuntimeError("LTFS configuration closure is invalid") from None
    try:
        expected_backend = Path(tape_backend).resolve(strict=True)
    except OSError:
        raise RuntimeError("LTFS configuration closure is invalid") from None
    try:
        configured_backend = plugins[("tape", "file")].resolve(strict=True)
    except (KeyError, OSError):
        raise RuntimeError("LTFS configuration closure is invalid") from None
    if configured_backend != expected_backend or defaults.get("tape") != "file":
        raise RuntimeError("LTFS configuration closure is invalid")
    scheduler_name = defaults.get("iosched")
    if scheduler_name not in {"fcfs", "unified"}:
        raise RuntimeError("LTFS configuration closure is invalid")
    expected_scheduler = Path(iosched_backend)
    try:
        configured_scheduler = plugins[("iosched", scheduler_name)]
        resolved_scheduler = expected_scheduler.resolve(strict=True)
    except (KeyError, OSError):
        raise RuntimeError("LTFS configuration closure is invalid") from None
    if (not expected_scheduler.is_absolute() or
            configured_scheduler != expected_scheduler or
            resolved_scheduler != expected_scheduler or
            not resolved_scheduler.is_file()):
        raise RuntimeError("LTFS configuration closure is invalid")
    for plugin_type, name in defaults.items():
        if name != "none":
            try:
                library = plugins[(plugin_type, name)].resolve(strict=True)
            except (KeyError, OSError):
                raise RuntimeError("LTFS configuration closure is invalid") from None
            if not library.is_file():
                raise RuntimeError("LTFS configuration closure is invalid")


def _hash_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(COPY_BUFFER_BYTES): digest.update(chunk)
    return digest.hexdigest()


def _mount_state(mountpoint, tools, commands):
    found = commands.run([str(tools.findmnt), "--mountpoint", str(mountpoint)])
    if found.returncode == 0:
        return True
    if found.returncode == 1:
        return False
    raise RuntimeError("findmnt failed")


def _wait_mount(mountpoint, tools, commands, expected, process=None):
    for _ in range(100):
        if _mount_state(mountpoint, tools, commands) == expected:
            return
        if process is not None and process.poll() is not None:
            raise RuntimeError("LTFS mount process exited before mount became ready")
        time.sleep(0.05)
    raise RuntimeError("LTFS mount state did not reach the required boundary")


def _mount(mountpoint, cartridge, receipt_path, tools, commands, readonly=False):
    options = ["tape_backend=file", f"devname={cartridge}",
               f"config_file={tools.config}", "sync_type=unmount", "noeject"]
    options.append(f"standalone_receipt={receipt_path}")
    if readonly: options.append("ro")
    operation_id = str(uuid.uuid4())
    process = commands.start([
        str(tools.ltfs), f"--operation-id={operation_id}", str(mountpoint),
        "-o", ",".join(options),
    ])
    try:
        _wait_mount(mountpoint, tools, commands, True, process)
    except Exception as primary_error:
        _raise_after_cleanup(primary_error, mountpoint, process, tools,
                             commands, force_lazy=True)
    return process


def _receipt(path, purpose, expected_bytes, expected_files, readonly=False):
    raw = path.read_bytes()
    try: data = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("standalone finalization receipt is invalid") from error
    expected_keys = {"schema", "stage", "operation_id", "volume_uuid", "prior_generation",
        "new_generation", "bytes_valid", "bytes", "files_valid", "files",
        "phase_duration_ns", "capture_duration_ns", "device_close_duration_ns",
        "device_close_result_valid", "device_close_result",
        "catalog_ack_duration_ns", "media_committed", "catalog_acknowledged",
        "cleanup_failed", "result"}
    uint_fields = ("prior_generation", "new_generation", "bytes", "files",
                   "capture_duration_ns", "device_close_duration_ns",
                   "catalog_ack_duration_ns")
    bool_fields = ("bytes_valid", "files_valid", "device_close_result_valid",
                   "media_committed", "catalog_acknowledged", "cleanup_failed")
    if (type(data) is not dict or set(data) != expected_keys or
            data["schema"] != 1 or data["stage"] != "terminal" or
            type(data["operation_id"]) is not str or not UUID.fullmatch(data["operation_id"]) or
            type(data["volume_uuid"]) is not str or not UUID.fullmatch(data["volume_uuid"]) or
            any(type(data[name]) is not int or data[name] < 0 for name in uint_fields) or
            any(type(data[name]) is not bool for name in bool_fields) or
            type(data["device_close_result"]) is not int or type(data["result"]) is not int or
            type(data["new_generation"]) is not int or data["new_generation"] <= 0 or
            data["new_generation"] < data["prior_generation"] or
            (readonly and data["new_generation"] != data["prior_generation"]) or
            data["bytes_valid"] is not True or data["bytes"] != expected_bytes or
            data["files_valid"] is not True or data["files"] != expected_files or
            type(data["phase_duration_ns"]) is not list or
            len(data["phase_duration_ns"]) != 11 or
            any(type(value) is not int or value < 0 for value in data["phase_duration_ns"]) or
            data["media_committed"] is not True or
            data["catalog_acknowledged"] is not True or
            data["device_close_result_valid"] is not True or
            data["device_close_result"] != 0 or
            data["cleanup_failed"] is not False or data["result"] != 0):
        raise RuntimeError("standalone finalization receipt contradicts the run")
    return {"purpose": purpose, "sha256": hashlib.sha256(raw).hexdigest(),
            "content_base64": base64.b64encode(raw).decode("ascii")}


def _unmount(mountpoint, process, tools, commands):
    _checked(commands.run([str(tools.fusermount), "-u", str(mountpoint)]),
             "LTFS unmount")
    if process.wait(timeout=30) != 0:
        raise RuntimeError("LTFS mount process failed during finalization")
    _wait_mount(mountpoint, tools, commands, False)


def _contain_process(process):
    try:
        process.wait(timeout=30)
        return
    except Exception:
        pass
    try:
        process.terminate()
    except Exception:
        pass
    try:
        process.wait(timeout=5)
        return
    except Exception:
        pass
    try:
        process.kill()
    except Exception:
        pass
    try:
        process.wait(timeout=5)
    except Exception as error:
        raise RuntimeError("LTFS cleanup failed to reap mount process") from error


def _cleanup_mount(mountpoint, process, tools, commands, force_lazy=False):
    def cleanup_run(argv):
        try:
            return commands.run(argv)
        except Exception:
            # Each cleanup action is independent and best effort.
            return None

    cleanup_run([str(tools.fusermount), "-u", str(mountpoint)])
    try:
        mounted = _mount_state(mountpoint, tools, commands)
    except Exception:
        mounted = None
    if force_lazy or mounted is not False:
        cleanup_run([str(tools.fusermount), "-u", "-z", str(mountpoint)])
        try:
            _mount_state(mountpoint, tools, commands)
        except Exception:
            pass
    try:
        _contain_process(process)
    except Exception as process_error:
        raise RuntimeError("LTFS cleanup failed") from process_error
    try:
        mounted = _mount_state(mountpoint, tools, commands)
    except Exception as state_error:
        raise RuntimeError("LTFS cleanup failed") from state_error
    if mounted:
        cleanup_run([str(tools.fusermount), "-u", "-z", str(mountpoint)])
        try:
            mounted = _mount_state(mountpoint, tools, commands)
        except Exception as state_error:
            raise RuntimeError("LTFS cleanup failed") from state_error
    if mounted:
        raise RuntimeError("LTFS cleanup failed: mount remains active")


def _raise_after_cleanup(primary_error, mountpoint, process, tools, commands,
                         force_lazy=False):
    try:
        _cleanup_mount(mountpoint, process, tools, commands, force_lazy)
    except Exception as cleanup_error:
        raise primary_error from cleanup_error
    raise primary_error


def _environment(source):
    cpu_model = None
    try:
        for line in Path("/proc/cpuinfo").read_text(errors="replace").splitlines():
            if line.lower().startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip(); break
    except OSError:
        pass
    ram = None
    try: ram = os.sysconf("SC_PHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    except (ValueError, OSError): pass
    return {"system": platform.system(), "release": platform.release(),
            "machine": platform.machine(), "cpu_count": os.cpu_count(),
            "cpu_model": cpu_model, "ram_bytes": ram,
            "source_filesystem_device": os.stat(source).st_dev,
            "hba": None, "driver": None, "firmware": None, "tape_alias": None}


def run(seed, large_bytes, tools, commands=None, *, _test_fusermount=None):
    commands = commands or Commands()
    root = Path(__file__).resolve().parents[1]
    if _test_fusermount is None:
        fusermount = PRODUCTION_FUSERMOUNT
        try:
            resolved_fusermount = fusermount.resolve(strict=True)
        except OSError:
            raise RuntimeError("production fusermount is invalid") from None
        if (not fusermount.is_absolute() or resolved_fusermount != fusermount or
                not fusermount.is_file() or not os.access(fusermount, os.X_OK)):
            raise RuntimeError("production fusermount is invalid")
    else:
        fusermount = Path(_test_fusermount).resolve()
    iosched_backend = Path(tools.iosched_backend)
    if not iosched_backend.is_absolute():
        iosched_backend = Path.cwd() / iosched_backend
    tools = Toolchain(*(Path(value).resolve() for value in tools))
    tools = tools._replace(iosched_backend=iosched_backend,
                           fusermount=fusermount)
    _require_tools(tools)
    _validate_config_closure(tools.config, tools.tape_backend,
                             tools.iosched_backend)
    build = _build(root, tools, commands)
    with tempfile.TemporaryDirectory(prefix="lto-ltfs-bench-") as temporary:
        owned = Path(temporary); source = owned / "source"
        cartridge = owned / "cartridge"; mountpoint = owned / "mount"
        source.mkdir(mode=0o700); cartridge.mkdir(mode=0o700); mountpoint.mkdir(mode=0o700)
        manifest = workloads.generate(source, seed, large_bytes=large_bytes)
        _checked(commands.run([str(tools.mkltfs), "--config", str(tools.config),
                               "--backend", "file",
                               "--device", str(cartridge), "--no-compression",
                               "--force", "--quiet"]), "file-tape format")
        write_receipt = owned / "write-receipt.json"
        read_receipt = owned / "read-receipt.json"
        process = _mount(mountpoint, cartridge, write_receipt, tools, commands)
        started = time.monotonic_ns()
        try:
            for item in manifest:
                with (source / item["name"]).open("rb") as reader, \
                     (mountpoint / item["name"]).open("xb") as writer:
                    shutil.copyfileobj(reader, writer, COPY_BUFFER_BYTES)
                    writer.flush(); os.fsync(writer.fileno())
            _unmount(mountpoint, process, tools, commands)
        except Exception as primary_error:
            _raise_after_cleanup(primary_error, mountpoint, process, tools,
                                 commands)
        duration_ns = time.monotonic_ns() - started
        if duration_ns <= 0: raise RuntimeError("benchmark duration is unavailable")
        total = sum(item["size_bytes"] for item in manifest)
        receipts = [_receipt(write_receipt, "write", total, len(manifest))]
        process = _mount(mountpoint, cartridge, read_receipt, tools, commands, readonly=True)
        try:
            validations = []
            for item in manifest:
                observed = _hash_file(mountpoint / item["name"])
                validations.append({"name": item["name"],
                    "expected_sha256": item["sha256"], "observed_sha256": observed,
                    "matches": observed == item["sha256"]})
            if not all(item["matches"] for item in validations):
                raise RuntimeError("LTFS remount hash validation failed")
            _unmount(mountpoint, process, tools, commands)
        except Exception as primary_error:
            _raise_after_cleanup(primary_error, mountpoint, process, tools,
                                 commands)
        receipts.append(_receipt(read_receipt, "readback", 0, 0,
                                 readonly=True))
        return {"schema": 1, "build": build, "environment": _environment(source),
            "backend": {"kind": "file", "implementation": "ltfs-libtape-file",
                        "physical_tape": False,
                        "tape_backend_path": str(tools.tape_backend)},
            "settings": {"seed": seed, "large_bytes": large_bytes,
                         "copy_buffer_bytes": COPY_BUFFER_BYTES,
                         "compression_enabled": False, "sync_type": "unmount",
                         "mount_readback_readonly": True},
            "workloads": manifest,
            "metrics": {"duration_ns": duration_ns, "total_bytes": total,
                        "file_count": len(manifest),
                        "throughput_bytes_per_second": total * 1_000_000_000 // duration_ns,
                        "retry_count": 0, "error_count": 0,
                        "finalization_count": 2},
            "validation": {"method": "ltfs-unmount-remount-index-hash",
                           "all_hashes_match": True, "files": validations,
                           "receipts": receipts}}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", required=True, type=int)
    parser.add_argument("--large-bytes", type=int, default=workloads.DEFAULT_LARGE_BYTES)
    parser.add_argument("--output", required=True, type=Path)
    for name in ("ltfs", "mkltfs", "tape-backend", "iosched-backend", "config",
                 "findmnt"):
        parser.add_argument("--" + name, required=True, type=Path)
    args = parser.parse_args()
    tools = Toolchain(args.ltfs, args.mkltfs, args.tape_backend,
                      args.iosched_backend, args.config, PRODUCTION_FUSERMOUNT,
                      args.findmnt)
    try: result = run(args.seed, args.large_bytes, tools)
    except RuntimeError as error:
        print(str(error), file=sys.stderr); return 2
    args.output.write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    return 0


if __name__ == "__main__": raise SystemExit(main())
