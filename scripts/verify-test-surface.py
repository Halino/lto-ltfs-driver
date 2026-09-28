#!/usr/bin/python3
"""Verify that built LTFS surfaces are mapped to failure-sensitive tests."""

from __future__ import annotations

import argparse
import ast
import json
import os
import re
import stat
import subprocess
import sys
from pathlib import Path

MAX_MANIFEST_BYTES = 1024 * 1024
TOP_KEYS = frozenset({"schema", "library", "commands", "exports"})
COMMAND_KEYS = frozenset({"artifact", "help_argv", "help_exit_codes", "options"})
SURFACE_KEYS = frozenset({"name", "operation_class", "evidence", "physical_policy"})
EXPORT_KEYS = frozenset(
    {"name", "operation_class", "evidence", "probe", "physical_policy"}
)
OPERATION_CLASSES = frozenset(
    {"read-only", "positioning", "write", "commit", "destructive"}
)
EXPORT_PROBES = frozenset({"abi-export", "source-call"})
REQUIRED_EXPORT_CLASSES = {
    "ltfs_format_tape": "destructive",
    "ltfs_unformat_tape": "destructive",
    "ltfs_eject_tape": "destructive",
    "ltfs_reset_capacity": "destructive",
    "tape_reset_capacity": "destructive",
    "set_tape_attribute": "write",
    "tape_set_attribute_to_cm": "write",
    "update_tape_attribute": "write",
    "tape_set_key": "write",
    "tape_clear_key": "write",
    "dcache_setxattr": "write",
    "ltfs_fsops_setxattr": "write",
    "ltfs_set_vendorunique_xattr": "write",
    "tape_set_vendorunique_xattr": "write",
    "xattr_do_set": "write",
    "xattr_set": "write",
}
REFUSAL_MARKERS = (
    "fail",
    "reject",
    "refus",
    "denied",
    "unauthor",
    "without",
    "wrong",
    "mismatch",
)
OPTION = re.compile(r"--[a-z0-9][a-z0-9-]*(?:=[^\s,;]+)?")
FUSE_OPTION = re.compile(r"(?:^|\s)-o\s+([a-z][a-z0-9_]*)(?:=[^\s,;]+)?")
SYMBOL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
C_TEST = re.compile(
    r"(?m)^\s*(?:static\s+)?int\s+(test_[A-Za-z0-9_]+)\s*\("
)


class SurfaceError(Exception):
    pass


def _regular_file(path: Path, *, executable: bool = False) -> None:
    try:
        result = path.lstat()
    except OSError:
        raise SurfaceError(f"artifact is unavailable: {path}") from None
    if not stat.S_ISREG(result.st_mode) or result.st_nlink != 1:
        raise SurfaceError(f"artifact must be a regular non-symlink file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise SurfaceError(f"artifact is not executable: {path}")


def _canonical_directory(path: Path, purpose: str) -> Path:
    if not path.is_absolute() or path.is_symlink():
        raise SurfaceError(f"{purpose} must be an absolute non-symlink directory")
    try:
        resolved = path.resolve(strict=True)
    except OSError:
        raise SurfaceError(f"{purpose} is unavailable") from None
    if resolved != path or not resolved.is_dir():
        raise SurfaceError(f"{purpose} must be a canonical directory")
    return resolved


def _closed_keys(value: dict, allowed: frozenset[str], purpose: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise SurfaceError(f"unknown manifest key in {purpose}: {unknown[0]}")


def _read_manifest(path: Path) -> dict:
    _regular_file(path)
    payload = path.read_bytes()
    if not payload or len(payload) > MAX_MANIFEST_BYTES:
        raise SurfaceError("manifest size is invalid")
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise SurfaceError("manifest JSON is invalid") from None
    if type(value) is not dict:
        raise SurfaceError("manifest root must be an object")
    _closed_keys(value, TOP_KEYS, "root")
    if value.get("schema") != 2:
        raise SurfaceError("manifest schema is unsupported")
    if type(value.get("library")) is not str:
        raise SurfaceError("manifest library is invalid")
    if (
        type(value.get("commands")) is not dict
        or type(value.get("exports")) is not list
    ):
        raise SurfaceError("manifest collections are invalid")
    return value


def _evidence_test(source_root: Path, reference: str) -> tuple[Path, str, object]:
    if type(reference) is not str or reference.count("::") != 1:
        raise SurfaceError("evidence reference is invalid")
    relative, test_name = reference.split("::", 1)
    candidate = Path(relative)
    if (
        candidate.is_absolute()
        or not candidate.parts
        or any(part in {"", ".", ".."} for part in candidate.parts)
        or not test_name.startswith("test_")
    ):
        raise SurfaceError("evidence reference is invalid")
    path = source_root.joinpath(*candidate.parts)
    _regular_file(path)
    try:
        source = path.read_text(encoding="utf-8")
        if path.suffix == ".py":
            tree = ast.parse(source, filename=os.fspath(path))
            tests = {
                node.name: node
                for node in ast.walk(tree)
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            }
        elif path.suffix == ".c":
            tests = {match.group(1): match for match in C_TEST.finditer(source)}
        else:
            raise SurfaceError(f"evidence source type is unsupported: {relative}")
    except (OSError, UnicodeDecodeError, SyntaxError):
        raise SurfaceError(f"evidence source is invalid: {relative}") from None
    if test_name not in tests:
        raise SurfaceError(f"evidence test is absent: {reference}")
    return path, source, tests[test_name]


def _evidence_exists(source_root: Path, reference: str) -> None:
    _evidence_test(source_root, reference)


def _c_test_body(source: str, match: re.Match[str]) -> str:
    opening = source.find("{", match.end())
    if opening < 0:
        raise SurfaceError("C evidence test body is invalid")
    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index + 1] if index + 1 < len(source) else ""
        if state == "code":
            if char == "/" and following == "/":
                state = "line-comment"
                index += 2
                continue
            if char == "/" and following == "*":
                state = "block-comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[opening : index + 1]
        elif state == "line-comment":
            if char == "\n":
                state = "code"
        elif state == "block-comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif char == "\\":
            index += 2
            continue
        elif (state == "string" and char == '"') or (
            state == "character" and char == "'"
        ):
            state = "code"
        index += 1
    raise SurfaceError("C evidence test body is invalid")


def _c_code_only(source: str) -> str:
    output = list(source)
    state = "code"
    index = 0
    while index < len(output):
        char = output[index]
        following = output[index + 1] if index + 1 < len(output) else ""
        if state == "code":
            if char == "/" and following == "/":
                output[index] = output[index + 1] = " "
                state = "line-comment"
                index += 2
                continue
            if char == "/" and following == "*":
                output[index] = output[index + 1] = " "
                state = "block-comment"
                index += 2
                continue
            if char == '"':
                output[index] = " "
                state = "string"
            elif char == "'":
                output[index] = " "
                state = "character"
        elif state == "line-comment":
            if char == "\n":
                state = "code"
            else:
                output[index] = " "
        elif state == "block-comment":
            output[index] = " "
            if char == "*" and following == "/":
                output[index + 1] = " "
                state = "code"
                index += 2
                continue
        else:
            output[index] = " "
            if char == "\\":
                if index + 1 < len(output):
                    output[index + 1] = " "
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    return "".join(output)


def _evidence_calls_symbol(source_root: Path, reference: str, symbol: str) -> None:
    path, source, test = _evidence_test(source_root, reference)
    if path.suffix == ".py":
        if not isinstance(test, (ast.FunctionDef, ast.AsyncFunctionDef)):
            raise SurfaceError("Python evidence test body is invalid")
        calls = {
            call.func.id
            if isinstance(call.func, ast.Name)
            else call.func.attr
            if isinstance(call.func, ast.Attribute)
            else None
            for call in ast.walk(test)
            if isinstance(call, ast.Call)
        }
        found = symbol in calls
    else:
        if not isinstance(test, re.Match):
            raise SurfaceError("C evidence test body is invalid")
        body = _c_code_only(_c_test_body(source, test))
        found = re.search(rf"\b{re.escape(symbol)}\s*\(", body) is not None
    if not found:
        raise SurfaceError(f"export evidence does not call symbol: {symbol}")


def _validate_surface(source_root: Path, value: object, purpose: str) -> str:
    if type(value) is not dict:
        raise SurfaceError(f"surface entry is invalid: {purpose}")
    _closed_keys(value, SURFACE_KEYS, purpose)
    name = value.get("name")
    operation_class = value.get("operation_class")
    evidence = value.get("evidence")
    physical_policy = value.get("physical_policy")
    if type(name) is not str or not name or "\x00" in name:
        raise SurfaceError(f"surface name is invalid: {purpose}")
    if operation_class not in OPERATION_CLASSES:
        raise SurfaceError(f"operation class is invalid: {purpose}")
    if (
        type(evidence) is not list
        or not evidence
        or not all(type(item) is str for item in evidence)
    ):
        raise SurfaceError(f"surface evidence is invalid: {purpose}")
    if len(evidence) != len(set(evidence)):
        raise SurfaceError(f"duplicate evidence: {purpose}")
    for reference in evidence:
        _evidence_exists(source_root, reference)
    if operation_class == "destructive":
        if type(physical_policy) is not str or not physical_policy:
            raise SurfaceError(
                f"destructive surface requires a physical policy: {purpose}"
            )
        if not any(
            marker in reference.casefold()
            for marker in REFUSAL_MARKERS
            for reference in evidence
        ):
            raise SurfaceError(
                f"destructive surface requires refusal evidence: {purpose}"
            )
    elif physical_policy is not None:
        raise SurfaceError(f"non-destructive surface has a physical policy: {purpose}")
    return name


def _validate_export(source_root: Path, value: object, purpose: str) -> tuple[str, str]:
    if type(value) is not dict:
        raise SurfaceError(f"surface entry is invalid: {purpose}")
    _closed_keys(value, EXPORT_KEYS, purpose)
    name = value.get("name")
    operation_class = value.get("operation_class")
    evidence = value.get("evidence")
    probe = value.get("probe")
    physical_policy = value.get("physical_policy")
    if type(name) is not str or not SYMBOL.fullmatch(name):
        raise SurfaceError(f"exported symbol name is invalid: {name}")
    if operation_class not in OPERATION_CLASSES:
        raise SurfaceError(f"operation class is invalid: {purpose}")
    required_class = REQUIRED_EXPORT_CLASSES.get(name)
    if required_class is not None and operation_class != required_class:
        raise SurfaceError(
            f"export {name} requires {required_class} operation class"
        )
    if type(evidence) is not list or not all(type(item) is str for item in evidence):
        raise SurfaceError(f"surface evidence is invalid: {purpose}")
    if len(evidence) != len(set(evidence)):
        raise SurfaceError(f"duplicate evidence: {purpose}")
    if probe not in EXPORT_PROBES:
        raise SurfaceError(f"export probe is invalid: {purpose}")
    if probe == "abi-export":
        if evidence:
            raise SurfaceError(f"ABI-only export must not claim test evidence: {name}")
    else:
        if not evidence:
            raise SurfaceError(f"source-call export requires test evidence: {name}")
        for reference in evidence:
            _evidence_calls_symbol(source_root, reference, name)
        if operation_class == "destructive" and not any(
            marker in reference.casefold()
            for marker in REFUSAL_MARKERS
            for reference in evidence
        ):
            raise SurfaceError(
                f"destructive source-call export requires refusal evidence: {name}"
            )
    if operation_class == "destructive":
        if type(physical_policy) is not str or not physical_policy:
            raise SurfaceError(
                f"destructive surface requires a physical policy: {purpose}"
            )
    elif physical_policy is not None:
        raise SurfaceError(f"non-destructive surface has a physical policy: {purpose}")
    return name, probe


def _relative_artifact(build_root: Path, value: object, purpose: str) -> Path:
    if type(value) is not str:
        raise SurfaceError(f"{purpose} path is invalid")
    relative = Path(value)
    if (
        relative.is_absolute()
        or not relative.parts
        or any(part in {"", ".", ".."} for part in relative.parts)
    ):
        raise SurfaceError(f"{purpose} path is invalid")
    return build_root.joinpath(*relative.parts)


def _cli_options(
    binary: Path,
    help_argv: list[str],
    help_exit_codes: set[int],
    source_root: Path,
    build_root: Path,
) -> set[str]:
    _regular_file(binary, executable=True)
    expanded_argv = []
    for argument in help_argv:
        expanded = argument.replace("{source_root}", os.fspath(source_root)).replace(
            "{build_root}", os.fspath(build_root)
        )
        if "{" in expanded or "}" in expanded or "\x00" in expanded:
            raise SurfaceError(f"help argv is invalid: {binary.name}")
        expanded_argv.append(expanded)
    environment = {"PATH": "/usr/bin:/bin", "LC_ALL": "C"}
    loader_paths = []
    built_library_directory = build_root / "src" / "libltfs" / ".libs"
    if built_library_directory.exists():
        if (
            not built_library_directory.is_dir()
            or built_library_directory.is_symlink()
        ):
            raise SurfaceError("build library directory is invalid")
        loader_paths.append(os.fspath(built_library_directory))
    inherited_loader_path = os.environ.get("LD_LIBRARY_PATH")
    if inherited_loader_path:
        loader_paths.append(inherited_loader_path)
    if loader_paths:
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(loader_paths)
    try:
        result = subprocess.run(
            [os.fspath(binary), *expanded_argv],
            text=True,
            capture_output=True,
            timeout=10,
            check=False,
            env=environment,
        )
    except (OSError, subprocess.TimeoutExpired):
        raise SurfaceError(f"CLI help failed: {binary.name}") from None
    if result.returncode not in help_exit_codes:
        raise SurfaceError(f"CLI help failed: {binary.name}")
    output = result.stdout + result.stderr
    found = set()
    for match in OPTION.findall(output):
        found.add(match.split("=", 1)[0])
    for match in FUSE_OPTION.findall(output):
        found.add(f"-o:{match}")
    return found


def _exports(library: Path) -> set[str]:
    _regular_file(library)
    result = subprocess.run(
        ["/usr/bin/nm", "-D", "--defined-only", os.fspath(library)],
        text=True,
        capture_output=True,
        timeout=10,
        check=False,
        env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"},
    )
    if result.returncode != 0:
        raise SurfaceError("unable to inspect libltfs exports")
    found = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and SYMBOL.fullmatch(fields[-1]):
            found.add(fields[-1])
    return found


def verify(
    manifest_path: Path, source_root: Path, build_root: Path
) -> tuple[int, int, int, int]:
    source_root = _canonical_directory(source_root, "source root")
    build_root = _canonical_directory(build_root, "build root")
    manifest = _read_manifest(manifest_path)

    mapped_options: set[tuple[str, str]] = set()
    actual_options: set[tuple[str, str]] = set()
    for command_name, command in manifest["commands"].items():
        if type(command_name) is not str or not SYMBOL.fullmatch(
            command_name.replace("-", "_")
        ):
            raise SurfaceError("command name is invalid")
        if type(command) is not dict:
            raise SurfaceError(f"command entry is invalid: {command_name}")
        _closed_keys(command, COMMAND_KEYS, f"command {command_name}")
        artifact = command.get("artifact")
        help_argv = command.get("help_argv")
        help_exit_codes = command.get("help_exit_codes")
        options = command.get("options")
        if (
            type(help_argv) is not list
            or not help_argv
            or not all(type(item) is str and item and "\x00" not in item for item in help_argv)
        ):
            raise SurfaceError(f"help argv is invalid: {command_name}")
        if (
            type(help_exit_codes) is not list
            or not help_exit_codes
            or not all(
                type(item) is int and 0 <= item <= 255 for item in help_exit_codes
            )
            or len(help_exit_codes) != len(set(help_exit_codes))
        ):
            raise SurfaceError(f"help exit codes are invalid: {command_name}")
        if type(options) is not list or not options:
            raise SurfaceError(f"command options are invalid: {command_name}")
        for index, option in enumerate(options):
            name = _validate_surface(
                source_root, option, f"command {command_name} option {index}"
            )
            if not re.fullmatch(r"(?:--[a-z0-9][a-z0-9-]*|-o:[a-z][a-z0-9_]*)", name):
                raise SurfaceError(f"CLI option name is invalid: {command_name}:{name}")
            key = (command_name, name)
            if key in mapped_options:
                raise SurfaceError(f"duplicate CLI option: {command_name}:{name}")
            mapped_options.add(key)
        binary = _relative_artifact(build_root, artifact, f"command {command_name}")
        for name in _cli_options(
            binary, help_argv, set(help_exit_codes), source_root, build_root
        ):
            actual_options.add((command_name, name))

    missing_options = sorted(actual_options - mapped_options)
    extra_options = sorted(mapped_options - actual_options)
    if missing_options:
        command, option = missing_options[0]
        raise SurfaceError(f"unmapped CLI option: {command}:{option}")
    if extra_options:
        command, option = extra_options[0]
        raise SurfaceError(f"manifest CLI option is absent: {command}:{option}")

    mapped_exports: set[str] = set()
    source_call_exports = 0
    abi_only_exports = 0
    for index, exported in enumerate(manifest["exports"]):
        name, probe = _validate_export(source_root, exported, f"export {index}")
        if name in mapped_exports:
            raise SurfaceError(f"duplicate exported symbol: {name}")
        mapped_exports.add(name)
        if probe == "source-call":
            source_call_exports += 1
        else:
            abi_only_exports += 1
    library = _relative_artifact(build_root, manifest["library"], "library")
    actual_exports = _exports(library)
    missing_exports = sorted(actual_exports - mapped_exports)
    extra_exports = sorted(mapped_exports - actual_exports)
    if missing_exports:
        raise SurfaceError(f"unmapped exported surface: {missing_exports[0]}")
    if extra_exports:
        raise SurfaceError(f"manifest exported surface is absent: {extra_exports[0]}")
    return (
        len(mapped_options),
        len(mapped_exports),
        source_call_exports,
        abi_only_exports,
    )


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--build-root", required=True, type=Path)
    parser.add_argument("--json", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        options, exports, source_calls, abi_only = verify(
            args.manifest, args.source_root, args.build_root
        )
    except SurfaceError as error:
        print(f"surface verification failed: {error}", file=sys.stderr)
        return 2
    if args.json:
        print(
            json.dumps(
                {
                    "schema": 1,
                    "verdict": "PASS",
                    "options": options,
                    "exports": {
                        "total": exports,
                        "source_call": source_calls,
                        "abi_export": abi_only,
                    },
                },
                sort_keys=True,
                separators=(",", ":"),
            )
        )
    else:
        print(
            f"surface verification passed: {options} options, {exports} exports "
            f"({source_calls} source-call, {abi_only} ABI-only)"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
