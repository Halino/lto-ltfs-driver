#!/usr/bin/python3
"""Plan and execute destructive LTFS qualification on the file backend only."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
import subprocess
import sys
import uuid
from collections import namedtuple
from pathlib import Path

OPERATIONS = (
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
)
OWNED_NAMES = ("cartridge", "mount", "receipts", "evidence.json")
DEFAULT_LABEL = "SYNTHETIC_TEST_LABEL"
Toolchain = namedtuple(
    "Toolchain",
    "ltfs mkltfs ltfsck tape_backend iosched_backend config fusermount findmnt",
)
TERMINAL_KEYS = frozenset(
    {
        "schema",
        "stage",
        "operation_id",
        "volume_uuid",
        "prior_generation",
        "new_generation",
        "bytes_valid",
        "bytes",
        "files_valid",
        "files",
        "phase_duration_ns",
        "capture_duration_ns",
        "device_close_duration_ns",
        "device_close_result_valid",
        "device_close_result",
        "catalog_ack_duration_ns",
        "media_committed",
        "catalog_acknowledged",
        "cleanup_failed",
        "result",
    }
)


class QualificationRefused(Exception):
    pass


def plan(label: str = DEFAULT_LABEL) -> dict:
    if type(label) is not str or not label or "\x00" in label:
        raise QualificationRefused("physical label is invalid")
    return {
        "schema": 1,
        "verdict": "PLAN",
        "backend": "file",
        "physical": False,
        "physical_label": label,
        "owned_names": list(OWNED_NAMES),
        "operations": list(OPERATIONS),
    }


def _require_virtual_cartridge(path: Path) -> Path:
    if not path.is_absolute() or path.is_symlink():
        raise QualificationRefused("virtual cartridge is invalid")
    try:
        metadata = os.stat(path, follow_symlinks=False)
        resolved = path.resolve(strict=True)
    except OSError:
        raise QualificationRefused("virtual cartridge is invalid") from None
    if resolved != path or not stat.S_ISDIR(metadata.st_mode):
        raise QualificationRefused("virtual cartridge must be an owned directory")
    return path


def _require_owned_root(path: Path) -> Path:
    if not path.is_absolute() or path.is_symlink():
        raise QualificationRefused("owned root is invalid")
    try:
        metadata = os.stat(path, follow_symlinks=False)
        resolved = path.resolve(strict=True)
    except OSError:
        raise QualificationRefused("owned root is invalid") from None
    if (
        resolved != path
        or not stat.S_ISDIR(metadata.st_mode)
        or stat.S_IMODE(metadata.st_mode) != 0o700
        or metadata.st_uid != os.geteuid()
    ):
        raise QualificationRefused("owned root must be a private canonical directory")
    return path


def _require_toolchain(tools: Toolchain) -> Toolchain:
    if type(tools) is not Toolchain:
        raise QualificationRefused("file-backend toolchain is invalid")
    for index, candidate in enumerate(tools):
        path = Path(candidate)
        try:
            metadata = path.lstat()
            resolved = path.resolve(strict=True)
        except OSError:
            raise QualificationRefused("file-backend toolchain is invalid") from None
        if (
            not path.is_absolute()
            or resolved != path
            or not stat.S_ISREG(metadata.st_mode)
            or metadata.st_nlink != 1
            or (index in {0, 1, 2, 6, 7} and not os.access(path, os.X_OK))
        ):
            raise QualificationRefused("file-backend toolchain is invalid")
    return tools


def _checked(result: subprocess.CompletedProcess, purpose: str) -> None:
    if result.returncode != 0:
        raise QualificationRefused(f"{purpose} failed")


def _hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _mount_state(mountpoint: Path, tools: Toolchain, commands) -> bool:
    result = commands.run(
        [os.fspath(tools.findmnt), "--mountpoint", os.fspath(mountpoint)]
    )
    if result.returncode not in {0, 1}:
        raise QualificationRefused("findmnt failed")
    return result.returncode == 0


def _mount(
    mountpoint: Path,
    cartridge: Path,
    receipt: Path,
    operation_id: str,
    tools: Toolchain,
    commands,
    *,
    readonly: bool,
):
    options = [
        "tape_backend=file",
        f"devname={cartridge}",
        f"config_file={tools.config}",
        "iosched_backend=fcfs",
        "sync_type=unmount",
        "noeject",
        f"standalone_receipt={receipt}",
    ]
    if readonly:
        options.append("ro")
    process = commands.start(
        [
            os.fspath(tools.ltfs),
            f"--operation-id={operation_id}",
            os.fspath(mountpoint),
            "-o",
            ",".join(options),
        ]
    )
    if not _mount_state(mountpoint, tools, commands):
        raise QualificationRefused("LTFS mount did not become ready")
    return process


def _terminal_receipt(path: Path, expected_operation: str) -> dict:
    try:
        metadata = path.lstat()
        raw = path.read_bytes()
        payload = json.loads(raw)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError):
        raise QualificationRefused("terminal receipt is invalid") from None
    uints = (
        "prior_generation",
        "new_generation",
        "bytes",
        "files",
        "capture_duration_ns",
        "device_close_duration_ns",
        "catalog_ack_duration_ns",
    )
    booleans = (
        "bytes_valid",
        "files_valid",
        "device_close_result_valid",
        "media_committed",
        "catalog_acknowledged",
        "cleanup_failed",
    )
    if (
        not stat.S_ISREG(metadata.st_mode)
        or stat.S_IMODE(metadata.st_mode) != 0o600
        or metadata.st_nlink != 1
        or type(payload) is not dict
        or set(payload) != TERMINAL_KEYS
        or payload["schema"] != 1
        or payload["stage"] != "terminal"
        or payload["operation_id"] != expected_operation
        or type(payload["volume_uuid"]) is not str
        or any(type(payload[name]) is not int or payload[name] < 0 for name in uints)
        or any(type(payload[name]) is not bool for name in booleans)
        or type(payload["phase_duration_ns"]) is not list
        or len(payload["phase_duration_ns"]) != 11
        or any(
            type(item) is not int or item < 0 for item in payload["phase_duration_ns"]
        )
        or type(payload["device_close_result"]) is not int
        or type(payload["result"]) is not int
        or not payload["media_committed"]
        or not payload["catalog_acknowledged"]
        or not payload["device_close_result_valid"]
        or payload["device_close_result"] != 0
        or payload["cleanup_failed"]
        or payload["result"] != 0
    ):
        raise QualificationRefused("terminal receipt contradicts the run")
    for suffix in (".pending", ".ready"):
        if Path(os.fspath(path) + suffix).exists():
            raise QualificationRefused("terminal receipt publication is incomplete")
    return {"sha256": hashlib.sha256(raw).hexdigest(), "payload": payload}


def _unmount(
    mountpoint: Path,
    process,
    receipt: Path,
    operation_id: str,
    tools: Toolchain,
    commands,
) -> dict:
    _checked(
        commands.run([os.fspath(tools.fusermount), "-u", os.fspath(mountpoint)]),
        "LTFS unmount",
    )
    if process.wait(timeout=30) != 0:
        raise QualificationRefused("LTFS process failed during finalization")
    if _mount_state(mountpoint, tools, commands):
        raise QualificationRefused("LTFS mount remains active")
    return _terminal_receipt(receipt, operation_id)


def _format(
    tools: Toolchain,
    commands,
    cartridge: Path,
    label: str,
    destructive_flag: str | None,
) -> None:
    argv = [
        os.fspath(tools.mkltfs),
        "--config",
        os.fspath(tools.config),
        "--backend",
        "file",
        "--device",
        os.fspath(cartridge),
        "--volume-name",
        label,
        "--tape-serial",
        "SYN001",
        "--no-compression",
        "--force",
        "--quiet",
    ]
    if destructive_flag:
        argv.append(destructive_flag)
    result = commands.run(argv)
    allowed = {0, 1} if destructive_flag in {"--wipe", "--long-wipe"} else {0}
    if result.returncode not in allowed:
        raise QualificationRefused("file-tape destructive format failed")


def run(owned_root: Path, cartridge: Path, label: str, tools, commands=None) -> dict:
    plan(label)
    owned_root = _require_owned_root(Path(owned_root))
    cartridge = _require_virtual_cartridge(Path(cartridge))
    if cartridge.parent != owned_root or cartridge.name != "cartridge":
        raise QualificationRefused("virtual cartridge is outside the owned root")
    tools = _require_toolchain(tools)
    if commands is None:
        commands = subprocess
    mountpoint = owned_root / "mount"
    receipts = owned_root / "receipts"
    for directory in (mountpoint, receipts):
        directory.mkdir(mode=0o700)
    evidence_receipts = []

    _format(tools, commands, cartridge, label, None)
    write_operation = str(uuid.uuid5(uuid.NAMESPACE_URL, "ltfs-file-destructive/write"))
    write_receipt = receipts / "write.json"
    process = _mount(
        mountpoint,
        cartridge,
        write_receipt,
        write_operation,
        tools,
        commands,
        readonly=False,
    )
    original = mountpoint / "alpha.bin"
    with original.open("xb") as stream:
        stream.write(b"alpha\n")
        stream.flush()
        os.fsync(stream.fileno())
    with original.open("ab") as stream:
        stream.write(b"beta\n")
        stream.flush()
        os.fsync(stream.fileno())
    renamed = mountpoint / "renamed.bin"
    original.rename(renamed)
    with renamed.open("r+b") as stream:
        stream.truncate(7)
        stream.flush()
        os.fsync(stream.fileno())
    deleted = mountpoint / "delete.bin"
    deleted.write_bytes(b"delete me")
    deleted.unlink()
    written_sha256 = _hash(renamed)
    evidence_receipts.append(
        _unmount(
            mountpoint,
            process,
            write_receipt,
            write_operation,
            tools,
            commands,
        )
    )

    _checked(
        commands.run(
            [
                os.fspath(tools.ltfsck),
                "--config",
                os.fspath(tools.config),
                "--backend",
                "file",
                os.fspath(cartridge),
            ]
        ),
        "file-tape ltfsck",
    )
    read_operation = str(uuid.uuid5(uuid.NAMESPACE_URL, "ltfs-file-destructive/read"))
    read_receipt = receipts / "read.json"
    process = _mount(
        mountpoint,
        cartridge,
        read_receipt,
        read_operation,
        tools,
        commands,
        readonly=True,
    )
    readback_sha256 = _hash(mountpoint / "renamed.bin")
    if readback_sha256 != written_sha256:
        raise QualificationRefused("readback hash mismatch")
    evidence_receipts.append(
        _unmount(
            mountpoint,
            process,
            read_receipt,
            read_operation,
            tools,
            commands,
        )
    )

    for destructive_flag in ("--wipe", "--long-wipe", "--destructive"):
        _format(tools, commands, cartridge, label, destructive_flag)

    empty_operation = str(uuid.uuid5(uuid.NAMESPACE_URL, "ltfs-file-destructive/empty"))
    empty_receipt = receipts / "empty.json"
    process = _mount(
        mountpoint,
        cartridge,
        empty_receipt,
        empty_operation,
        tools,
        commands,
        readonly=True,
    )
    if any(mountpoint.iterdir()):
        raise QualificationRefused("destructive reformat left visible files")
    evidence_receipts.append(
        _unmount(
            mountpoint,
            process,
            empty_receipt,
            empty_operation,
            tools,
            commands,
        )
    )
    return {
        **plan(label),
        "verdict": "PASS",
        "written_sha256": written_sha256,
        "readback_sha256": readback_sha256,
        "receipts": evidence_receipts,
        "owned_paths": [
            os.fspath(cartridge),
            os.fspath(mountpoint),
            os.fspath(receipts),
            os.fspath(owned_root / "evidence.json"),
        ],
    }


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--label", default=DEFAULT_LABEL)
    parser.add_argument("--execute-file-backend", action="store_true")
    parser.add_argument("--owned-root", type=Path)
    parser.add_argument("--cartridge-directory", type=Path)
    for name in (
        "ltfs",
        "mkltfs",
        "ltfsck",
        "tape-backend",
        "iosched-backend",
        "config",
        "fusermount",
        "findmnt",
    ):
        parser.add_argument(f"--{name}", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.execute_file_backend:
            values = (
                args.ltfs,
                args.mkltfs,
                args.ltfsck,
                args.tape_backend,
                args.iosched_backend,
                args.config,
                args.fusermount,
                args.findmnt,
            )
            if (
                args.owned_root is None
                or args.cartridge_directory is None
                or any(value is None for value in values)
            ):
                raise QualificationRefused("execution requires the complete toolchain")
            if args.output != args.owned_root / "evidence.json":
                raise QualificationRefused("evidence path is outside the owned root")
            result = run(
                args.owned_root,
                args.cartridge_directory,
                args.label,
                Toolchain(*values),
            )
        else:
            result = plan(args.label)
        args.output.write_text(
            json.dumps(result, sort_keys=True, indent=2) + "\n", encoding="utf-8"
        )
    except (OSError, QualificationRefused) as error:
        print(f"qualification refused: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
