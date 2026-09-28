#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
script_path=$script_dir/$(basename -- "$0")

if test "${1-}" = --internal-gcov; then
	test "$#" -eq 4 || exit 2
	coverage_source=$2
	coverage_output=$3
	coverage_build=$4
	test -x "$coverage_source/configure" || exit 2
	mkdir "$coverage_build"
	(
		cd "$coverage_build"
		/usr/bin/env \
			CFLAGS='-O0 -g --coverage' \
			LDFLAGS='--coverage' \
			"$coverage_source/configure" \
			--enable-tests --disable-snmp --disable-lintape
		/usr/bin/make -j2
		/usr/bin/make check
		/usr/bin/gcovr \
			--root "$coverage_source" \
			--object-directory "$coverage_build" \
			--json-pretty \
			--output "$coverage_output"
	)
	exit 0
fi

usage()
{
	printf '%s\n' \
		'usage: run-complete-qualification.sh --source-root DIR --build-root DIR --output FILE [--test-fixture-dir DIR]' >&2
	exit 2
}

source_root=
build_root=
output=
fixture_dir=
while test "$#" -gt 0; do
	case "$1" in
	--source-root|--build-root|--output|--test-fixture-dir)
		test "$#" -ge 2 || usage
		case "$1" in
		--source-root) source_root=$2 ;;
		--build-root) build_root=$2 ;;
		--output) output=$2 ;;
		--test-fixture-dir) fixture_dir=$2 ;;
		esac
		shift 2
		;;
	*) usage ;;
	esac
done
test -n "$source_root" && test -n "$build_root" && test -n "$output" || usage

canonical_dir()
{
	requested=$1
	test -d "$requested" && test ! -L "$requested" || return 1
	resolved=$(CDPATH= cd -- "$requested" && pwd -P) || return 1
	test "$resolved" = "$requested" || return 1
	printf '%s\n' "$resolved"
}

source_root=$(canonical_dir "$source_root") || {
	printf '%s\n' 'source root must be a canonical non-symlink directory' >&2
	exit 2
}
build_root=$(canonical_dir "$build_root") || {
	printf '%s\n' 'build root must be a canonical non-symlink directory' >&2
	exit 2
}
case "$output" in /*) ;; *) printf '%s\n' 'output must be absolute' >&2; exit 2 ;; esac
test ! -L "$output" || {
	printf '%s\n' 'output must not be a symlink' >&2
	exit 2
}
output_parent=$(dirname -- "$output")
output_parent=$(canonical_dir "$output_parent") || {
	printf '%s\n' 'output parent must be a canonical non-symlink directory' >&2
	exit 2
}
output=$output_parent/$(basename -- "$output")

private_dir=$(mktemp -d "${TMPDIR:-/tmp}/lto-ltfs-complete.XXXXXX")
trap 'rm -rf -- "$private_dir"' EXIT HUP INT TERM
chmod 700 "$private_dir"

record()
{
	action=$1
	shift
	/usr/bin/python3 - "$output" "$action" "$@" <<'PY'
import json
from pathlib import Path
import sys

path = Path(sys.argv[1])
action = sys.argv[2]
if action == "init":
    value = {
        "schema": 1,
        "verdict": "RUNNING",
        "hardware_free": True,
        "surface": None,
        "phases": [],
        "reason": None,
    }
else:
    value = json.loads(path.read_text(encoding="utf-8"))
    if action == "phase":
        name, status, exit_code = sys.argv[3], sys.argv[4], int(sys.argv[5])
        duration_seconds = int(sys.argv[6])
        value["phases"].append(
            {
                "name": name,
                "status": status,
                "exit_code": exit_code,
                "duration_seconds": duration_seconds,
            }
        )
    elif action == "surface":
        surface_path = Path(sys.argv[3])
        surface = json.loads(surface_path.read_text(encoding="utf-8"))
        if set(surface) != {"schema", "verdict", "options", "exports"}:
            raise ValueError("surface result has unknown or missing fields")
        exports = surface["exports"]
        if (
            surface["schema"] != 1
            or surface["verdict"] != "PASS"
            or type(surface["options"]) is not int
            or surface["options"] <= 0
            or type(exports) is not dict
            or set(exports) != {"total", "source_call", "abi_export"}
            or any(type(exports[key]) is not int for key in exports)
            or exports["total"] <= 0
            or exports["source_call"] <= 0
            or exports["abi_export"] <= 0
        ):
            raise ValueError("surface result lacks ABI or source-call coverage")
        if exports["total"] != exports["source_call"] + exports["abi_export"]:
            raise ValueError("surface probe totals are incoherent")
        value["surface"] = {
            "options": surface["options"],
            "exports": exports,
        }
    elif action == "finish":
        value["verdict"] = sys.argv[3]
        value["reason"] = None if len(sys.argv) == 4 else sys.argv[4]
    else:
        raise ValueError("unknown record action")
import os
import tempfile
with tempfile.NamedTemporaryFile(
    mode="w",
    encoding="utf-8",
    dir=path.parent,
    prefix=path.name + ".",
    delete=False,
) as sink:
    temporary = Path(sink.name)
    sink.write(json.dumps(value, sort_keys=True, indent=2) + "\n")
    sink.flush()
    os.fsync(sink.fileno())
temporary.chmod(0o600)
os.replace(temporary, path)
PY
}

record init

fail_phase()
{
	name=$1
	rc=$2
	reason=$3
	duration=${4:-0}
	record phase "$name" FAIL "$rc" "$duration"
	record finish FAIL "$reason"
	printf 'complete qualification failed in %s: %s\n' "$name" "$reason" >&2
	exit "$rc"
}

run_phase()
{
	name=$1
	shift
	stdout=$private_dir/$name.stdout
	stderr=$private_dir/$name.stderr
	started=$(/usr/bin/date +%s)
	if "$@" >"$stdout" 2>"$stderr"; then
		finished=$(/usr/bin/date +%s)
		record phase "$name" PASS 0 "$((finished - started))"
	else
		rc=$?
		finished=$(/usr/bin/date +%s)
		cat "$stderr" >&2
		fail_phase "$name" "$rc" "phase command failed" "$((finished - started))"
	fi
}

if test -n "$fixture_dir"; then
	fixture_dir=$(canonical_dir "$fixture_dir") || {
		printf '%s\n' 'fixture directory must be canonical' >&2
		exit 2
	}
	for phase in surface-verification unit-tests file-backend-destructive sanitizers gcov-export; do
		test -f "$fixture_dir/$phase" && test ! -L "$fixture_dir/$phase" && test -x "$fixture_dir/$phase" || {
			printf 'invalid fixture phase: %s\n' "$phase" >&2
			exit 2
		}
	done
	surface_command=$fixture_dir/surface-verification
else
	test -x "$source_root/scripts/verify-test-surface.py" || {
		printf '%s\n' 'surface verifier is unavailable' >&2
		exit 2
	}
	surface_command=$source_root/scripts/verify-test-surface.py
fi

surface_stdout=$private_dir/surface.json
surface_stderr=$private_dir/surface.stderr
surface_started=$(/usr/bin/date +%s)
if test -n "$fixture_dir"; then
	if "$surface_command" >"$surface_stdout" 2>"$surface_stderr"; then rc=0; else rc=$?; fi
else
	if "$surface_command" \
		--manifest "$source_root/qualification/ltfs-surface-v1.json" \
		--source-root "$source_root" \
		--build-root "$build_root" \
		--json >"$surface_stdout" 2>"$surface_stderr"; then rc=0; else rc=$?; fi
fi
if test "$rc" -ne 0; then
	cat "$surface_stderr" >&2
	surface_finished=$(/usr/bin/date +%s)
	fail_phase surface-verification "$rc" 'surface verifier failed' "$((surface_finished - surface_started))"
fi
if record surface "$surface_stdout" 2>"$surface_stderr"; then
	surface_finished=$(/usr/bin/date +%s)
	record phase surface-verification PASS 0 "$((surface_finished - surface_started))"
else
	cat "$surface_stderr" >&2
	surface_finished=$(/usr/bin/date +%s)
	fail_phase surface-verification 2 "$(tail -n 1 "$surface_stderr")" "$((surface_finished - surface_started))"
fi

cd "$source_root"

if test -n "$fixture_dir"; then
	run_phase unit-tests "$fixture_dir/unit-tests"
	run_phase file-backend-destructive "$fixture_dir/file-backend-destructive"
	run_phase sanitizers "$fixture_dir/sanitizers"
	run_phase gcov-export "$fixture_dir/gcov-export"
else
	run_phase unit-tests /usr/bin/make -C "$build_root" check
	run_phase file-backend-destructive /usr/bin/python3 -m unittest tests.python.test_file_destructive
	run_phase sanitizers /usr/bin/env -i \
		PATH=/usr/bin:/bin LC_ALL=C \
		"$source_root/scripts/run-sanitizer-qualification.sh"
	run_phase gcov-export "$script_path" --internal-gcov \
		"$source_root" \
		"$build_root/complete-qualification-coverage.json" \
		"$private_dir/coverage-build"
fi

record finish PASS
printf '%s\n' "complete hardware-free qualification passed: $output"
