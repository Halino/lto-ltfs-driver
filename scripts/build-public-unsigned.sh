#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Run only inside the digest-pinned, keyless EL9 GitHub build container.
set -euo pipefail

if (($# != 6)); then
    printf 'usage: %s TAG COMMIT BUNDLE LOCK ICU_TOOLS_DIR NEW_OUTPUT\n' "$0" >&2
    exit 2
fi
tag=$1
commit=$2
bundle=$3
lock=$4
icu_tools=$5
output=$6
script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repository=$(cd -- "$script_directory/.." && pwd -P)
if [[ "$output" != /* || -e "$output" || -L "$output" ]]; then
    printf 'unsigned output must be a new absolute path\n' >&2
    exit 1
fi
parent=$(dirname -- "$output")
if [[ ! -d "$parent" || -L "$parent" ]]; then
    printf 'unsigned output parent is unsafe\n' >&2
    exit 1
fi
for tool in /usr/bin/python3 /usr/bin/rpm /usr/bin/gpg /usr/bin/rpmkeys; do
    if [[ ! -x "$tool" ]]; then
        printf 'required pinned-container tool is missing: %s\n' "$tool" >&2
        exit 1
    fi
done
/usr/bin/python3 -B "$repository/scripts/verify-public-build.py" \
    --authenticate-bundle --bundle "$bundle" --lock "$lock"
/usr/bin/python3 -B "$repository/scripts/verify-public-build.py" \
    --authenticate-icu-tools "$icu_tools" --lock "$repository/packaging/rpm/icu-build-tools.json"
# The pinned UBI bootstrap may have installed exactly one authenticated cpio
# RPM to extract the ICU provider. Admit that narrow case only after the full
# bundle is reauthenticated; never skip any other dependency in the transaction.
if [[ "${LTO_CPIO_PREINSTALLED:-0}" == 1 ]]; then
    cpio_rpm="$bundle/build/cpio-2.13-16.el9.x86_64.rpm"
    if [[ ! -f "$cpio_rpm" || -L "$cpio_rpm" ]]; then
        printf 'reviewed cpio bootstrap RPM is absent\n' >&2
        exit 1
    fi
    expected=$(/usr/bin/rpm -qp --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}' "$cpio_rpm")
    installed=$(/usr/bin/rpm -q --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}' cpio)
    if [[ "$expected" != 'cpio-2.13-16.el9.x86_64' || "$installed" != "$expected" ]] || \
        [[ -n "$(/usr/bin/rpm -V cpio)" ]]; then
        printf 'installed bootstrap cpio differs from authenticated bundle\n' >&2
        exit 1
    fi
    build_rpms=()
    for package in "$bundle"/build/*.rpm; do
        if [[ "$package" != "$cpio_rpm" ]]; then
            build_rpms+=("$package")
        fi
    done
    if ((${#build_rpms[@]} == 0)); then
        printf 'authenticated build dependency closure is empty\n' >&2
        exit 1
    fi
    /usr/bin/rpm -Uvh --quiet "${build_rpms[@]}"
elif [[ "${LTO_CPIO_PREINSTALLED:-0}" == 0 ]]; then
    /usr/bin/rpm -Uvh --quiet "$bundle"/build/*.rpm
else
    printf 'invalid cpio bootstrap admission mode\n' >&2
    exit 1
fi
for tool in /usr/bin/git /usr/bin/gzip /usr/bin/rpmbuild \
            /usr/bin/sha256sum /usr/bin/mktemp; do
    if [[ ! -x "$tool" ]]; then
        printf 'required pinned-container tool is missing: %s\n' "$tool" >&2
        exit 1
    fi
done
epoch=$(/usr/bin/python3 -B "$repository/scripts/verify-public-build.py" \
    --repo "$repository" --tag "$tag" --commit "$commit" \
    --bundle "$bundle" --lock "$lock")
if [[ ! "$epoch" =~ ^[0-9]+$ ]]; then
    printf 'source date from approved tag is invalid\n' >&2
    exit 1
fi
bash "$repository/scripts/verify-icu-catalogs.sh" "$icu_tools" "$repository"
work=$(/usr/bin/mktemp -d "$parent/.lto-driver-unsigned.XXXXXXXX")
cleanup() {
    if [[ -n "${work:-}" && -d "$work" ]]; then
        rm -rf -- "$work"
    fi
}
trap cleanup EXIT HUP INT TERM
build=$work/rpmbuild
stage=$work/stage
mkdir -p "$build/SOURCES" "$build/SPECS" "$build/BUILD" \
    "$build/BUILDROOT" "$build/RPMS" "$build/SRPMS" "$stage"
source_archive=$build/SOURCES/lto-ltfs-0.1.2.tar.gz
/usr/bin/git -C "$repository" archive --format=tar \
    --prefix=lto-ltfs-0.1.2/ "$commit" | /usr/bin/gzip -n > "$source_archive"
cp -- "$repository/packaging/rpm/lto-ltfs.spec" "$build/SPECS/"
cp -- "$repository/packaging/udev/99-lto-ltfs.rules" "$build/SOURCES/"
cp -- "$repository/packaging/tmpfiles/lto-ltfs.conf" "$build/SOURCES/"
cp -- "$source_archive" "$stage/"
/usr/bin/python3 -B "$repository/scripts/verify-rpm.py" \
    --write-source-manifest "$stage/lto-ltfs-0.1.2.tar.gz" \
    "$stage/SOURCE-MANIFEST.json"
PATH="$icu_tools:/usr/bin:/bin" LD_BIND_NOW=1 SOURCE_DATE_EPOCH=$epoch /usr/bin/rpmbuild -ba \
    --define "_topdir $build" --define "source_date_epoch $epoch" \
    "$build/SPECS/lto-ltfs.spec"
binary=$build/RPMS/x86_64/lto-ltfs-0.1.2-22.el9.x86_64.rpm
source_rpm=$build/SRPMS/lto-ltfs-0.1.2-22.el9.src.rpm
if [[ ! -f "$binary" || -L "$binary" || ! -f "$source_rpm" || -L "$source_rpm" ]]; then
    printf 'exact release-22 RPM/SRPM was not produced\n' >&2
    exit 1
fi
mapfile -t built_files < <(find "$build/RPMS" "$build/SRPMS" -type f -printf '%P\n' | LC_ALL=C sort)
if ((${#built_files[@]} != 2)) || \
    [[ "${built_files[0]}" != 'lto-ltfs-0.1.2-22.el9.src.rpm' || \
       "${built_files[1]}" != 'x86_64/lto-ltfs-0.1.2-22.el9.x86_64.rpm' ]]; then
    printf 'rpmbuild emitted extra or unexpected package files\n' >&2
    exit 1
fi
cp -- "$binary" "$source_rpm" "$stage/"
/usr/bin/python3 -B "$repository/scripts/verify-public-build.py" \
    --lock "$repository/packaging/rpm/icu-build-tools.json" \
    --write-build-inputs "$stage/BUILD-INPUTS.json"
/usr/bin/rpm -qp --qf '%{PAYLOADDIGESTALGO}:%{PAYLOADDIGEST}\n' \
    "$stage/$(basename -- "$binary")" > "$stage/RPM-PAYLOAD-DIGEST"
(
    cd -- "$stage"
    /usr/bin/sha256sum BUILD-INPUTS.json RPM-PAYLOAD-DIGEST SOURCE-MANIFEST.json \
        lto-ltfs-0.1.2-22.el9.src.rpm \
        lto-ltfs-0.1.2-22.el9.x86_64.rpm \
        lto-ltfs-0.1.2.tar.gz > SHA256SUMS
)
/usr/bin/python3 -B "$repository/scripts/verify-public-build.py" \
    --repo "$repository" --tag "$tag" --commit "$commit" --output "$stage"
mv -- "$stage" "$output"
printf 'unsigned release-22 artifacts verified at %s\n' "$output"
