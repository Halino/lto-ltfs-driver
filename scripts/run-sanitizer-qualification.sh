#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repository_root=$(CDPATH= cd -- "$script_dir/.." && pwd)
cd "$repository_root"

./autogen.sh
if test -f Makefile; then
	make distclean
fi

# Autoconf probes intentionally exercise allocation edge cases that are not
# product leak tests. Keep ASan and UBSan instrumentation enabled while
# disabling only LeakSanitizer for every sanitized qualification phase.
ASAN_OPTIONS=detect_leaks=0
export ASAN_OPTIONS
# Undefined behavior must fail qualification, even if the caller omitted or
# disabled fail-fast reporting. A successful exit with diagnostics is not a pass.
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
export UBSAN_OPTIONS

CC='gcc -fsanitize=address,undefined' \
CFLAGS='-O1 -g -fno-omit-frame-pointer' \
	./configure --enable-tests --disable-snmp --disable-lintape

make -j"${LTFS_QUALIFICATION_JOBS:-2}"
make check

check_version()
{
	version_option=$1
	stdout_file=$(mktemp "${TMPDIR:-/tmp}/lto-ltfs-version-stdout.XXXXXX")
	stderr_file=$(mktemp "${TMPDIR:-/tmp}/lto-ltfs-version.XXXXXX")
	expected_file=$(mktemp "${TMPDIR:-/tmp}/lto-ltfs-version-expected.XXXXXX")
	trap 'rm -f "$stdout_file" "$stderr_file" "$expected_file"' EXIT HUP INT TERM
	printf 'lto-ltfs 0.1.1\n' >"$expected_file"
	if ! ./src/ltfs "$version_option" >"$stdout_file" 2>"$stderr_file"; then
		cat "$stderr_file" >&2
		rm -f "$stdout_file" "$stderr_file" "$expected_file"
		return 1
	fi
	if test -s "$stderr_file"; then
		cat "$stderr_file" >&2
		rm -f "$stdout_file" "$stderr_file" "$expected_file"
		return 1
	fi
	if ! cmp -s "$expected_file" "$stdout_file"; then
		printf '%s\n' "unexpected $version_option output:" >&2
		cat "$stdout_file" >&2
		rm -f "$stdout_file" "$stderr_file" "$expected_file"
		return 1
	fi
	rm -f "$stdout_file" "$stderr_file" "$expected_file"
	trap - EXIT HUP INT TERM
}

check_version --version
check_version -V
