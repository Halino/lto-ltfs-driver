#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Execute only in the pinned UBI build closure, never on a tape host.
set -euo pipefail

if (($# != 2)); then
    printf 'usage: %s TOOLS_DIR SOURCE_DIR\n' "$0" >&2
    exit 2
fi
tools=$1
source=$2
if [[ "$tools" != /* || "$source" != /* || ! -d "$tools" || -L "$tools" || ! -d "$source" || -L "$source" ]]; then
    printf 'ICU catalog probe paths are unsafe\n' >&2
    exit 1
fi
for tool in genrb pkgdata; do
    if [[ ! -f "$tools/$tool" || -L "$tools/$tool" || ! -x "$tools/$tool" ]]; then
        printf 'reviewed ICU tool is unavailable: %s\n' "$tool" >&2
        exit 1
    fi
done
for helper in /usr/bin/python3 /usr/bin/readelf /usr/bin/ldd /usr/bin/rpm \
              /usr/bin/pkgconf /usr/bin/icu-config /usr/bin/gcc /usr/bin/ar \
              /usr/bin/ranlib /usr/bin/autoreconf /usr/bin/mktemp; do
    if [[ ! -x "$helper" ]]; then
        printf 'locked UBI helper is unavailable: %s\n' "$helper" >&2
        exit 1
    fi
done
if [[ -e /usr/bin/genrb || -e /usr/bin/pkgdata ]]; then
    printf 'system ICU executables would bypass the isolated tool pair\n' >&2
    exit 1
fi
if [[ ! -f /usr/lib64/icu/67.1/pkgdata.inc || -L /usr/lib64/icu/67.1/pkgdata.inc ]]; then
    printf 'locked UBI pkgdata.inc is unavailable\n' >&2
    exit 1
fi
test "$(/usr/bin/rpm -q libicu)" = libicu-67.1-10.el9_6.x86_64
test "$(/usr/bin/rpm -q libicu-devel)" = libicu-devel-67.1-10.el9_6.x86_64
test "$(/usr/bin/rpm -qf /usr/lib64/icu/67.1/pkgdata.inc)" = libicu-devel-67.1-10.el9_6.x86_64
test "$(/usr/bin/rpm -qf /usr/bin/icu-config)" = libicu-devel-67.1-10.el9_6.x86_64
test "$(/usr/bin/rpm -qf /usr/bin/pkgconf)" = pkgconf-1.7.3-10.el9.x86_64
test "$(/usr/bin/rpm -qf /usr/bin/gcc)" = gcc-11.5.0-14.el9.x86_64
test "$(/usr/bin/rpm -qf /usr/bin/ar)" = binutils-2.35.2-72.el9.x86_64
test "$(/usr/bin/pkgconf --modversion icu-uc)" = 67.1

/usr/bin/python3 - "$source/scripts/prepare-icu-build-tools.py" <<'PY'
import runpy, sys
from pathlib import Path
gate = runpy.run_path(sys.argv[1])
gate["validate_pkgdata_config"](Path("/usr/lib64/icu/67.1/pkgdata.inc").read_text())
PY
for tool in "$tools/genrb" "$tools/pkgdata"; do
    /usr/bin/readelf -d "$tool" | grep -q 'Shared library: \[libicu'
    LD_BIND_NOW=1 /usr/bin/python3 - "$tool" "$source/scripts/prepare-icu-build-tools.py" <<'PY'
import runpy, subprocess, sys
gate = runpy.run_path(sys.argv[2])
report = subprocess.run(["/usr/bin/ldd", sys.argv[1]], check=True,
                        capture_output=True, text=True).stdout
for library in gate["validate_ubi_loader_report"](report):
    owner = subprocess.run(["/usr/bin/rpm", "-qf", library], check=True,
                           capture_output=True, text=True).stdout.strip()
    if owner != "libicu-67.1-10.el9_6.x86_64":
        raise SystemExit("ICU tool loaded a non-UBI library")
PY
done

scratch=$(/usr/bin/mktemp -d /tmp/lto-icu-catalog.XXXXXXXX)
cleanup() {
    if [[ -n "${scratch:-}" && -d "$scratch" ]]; then
        rm -rf -- "$scratch"
    fi
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$scratch/source" "$scratch/first" "$scratch/second"
cp -a "$source"/. "$scratch/source"/
(
    cd "$scratch/source"
    PATH="$tools:/usr/bin:/bin" /usr/bin/autoreconf -fi
    PATH="$tools:/usr/bin:/bin" ./configure --prefix=/usr --libdir=/usr/lib64 \
        --enable-fast --enable-tests --disable-snmp --disable-lintape >/dev/null
)
for directory in first second; do
    cp -a "$source/messages" "$scratch/$directory/messages"
    (
        cd "$scratch/$directory/messages"
        PATH="$tools:/usr/bin:/bin" LD_BIND_NOW=1 LC_ALL=C \
            ./make_message_src.sh libinternal_error_dat.a
    )
done
/usr/bin/python3 - "$scratch/first/messages/libinternal_error_dat.a" \
    "$scratch/second/messages/libinternal_error_dat.a" \
    "$source/scripts/prepare-icu-build-tools.py" <<'PY'
import runpy, sys
from pathlib import Path
gate = runpy.run_path(sys.argv[3])
gate["verify_catalog_pair"](Path(sys.argv[1]), Path(sys.argv[2]))
PY
printf 'ICU UBI loader, configure and repeatable catalog probe passed\n'
