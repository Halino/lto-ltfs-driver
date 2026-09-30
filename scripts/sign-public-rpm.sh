#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Sign only a compared driver candidate in the protected GitHub job.
set -euo pipefail
umask 077

if (($# != 6)); then
    printf 'usage: %s UNSIGNED NEW_OUTPUT SECRET_KEY PUBLIC_KEY PRIMARY_FPR SUBKEY_FPR\n' "$0" >&2
    exit 2
fi
unsigned=$1
output=$2
secret=$3
public=$4
primary=$5
subkey=$6
binary=lto-ltfs-0.1.1-22.el9.x86_64.rpm
source_rpm=lto-ltfs-0.1.1-22.el9.src.rpm
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)

if [[ "$output" != /* || -e "$output" || -L "$output" || ! -d "$(dirname -- "$output")" || -L "$(dirname -- "$output")" ]]; then
    printf 'signed candidate output must be a new absolute path\n' >&2
    exit 1
fi
if [[ ! "$primary" =~ ^[0-9A-F]{40}$ || ! "$subkey" =~ ^[0-9A-F]{40}$ || "$primary" == "$subkey" ]]; then
    printf 'full approved GPG fingerprints are required\n' >&2
    exit 1
fi
if [[ -L "$secret" || ! -f "$secret" || -L "$public" || ! -f "$public" ]]; then
    printf 'signing key inputs are missing or unsafe\n' >&2
    exit 1
fi
python3 -B "$repository/scripts/verify-public-build.py" \
    --compare-first "$unsigned" --compare-second "$unsigned"

mapfile -t public_fprs < <(gpg --batch --with-colons --show-keys "$public" | awk -F: '$1=="fpr" {print $10}')
if ((${#public_fprs[@]} < 2)) || [[ "${public_fprs[0]}" != "$primary" ]] || \
   [[ "${public_fprs[1]}" != "$subkey" ]]; then
    printf 'public key primary or subkey differs from approval\n' >&2
    exit 1
fi

parent=$(dirname -- "$output")
stage=$(mktemp -d "$parent/.lto-driver-signing.XXXXXXXX")
cleanup() {
    if [[ -n "${stage:-}" && -d "$stage" ]]; then
        rm -rf -- "$stage"
    fi
}
trap cleanup EXIT HUP INT TERM
mkdir -m 700 "$stage/gnupg" "$stage/rpmdb" "$stage/payload"
gpg --homedir "$stage/gnupg" --batch --quiet --import "$secret"
gpg --homedir "$stage/gnupg" --batch --list-secret-keys "$subkey" >/dev/null
rpm --dbpath "$stage/rpmdb" --initdb
rpmkeys --dbpath "$stage/rpmdb" --import "$public"

for name in "$source_rpm" "$binary" \
            lto-ltfs-0.1.1.tar.gz SOURCE-MANIFEST.json \
            BUILD-INPUTS.json RPM-PAYLOAD-DIGEST; do
    cp -- "$unsigned/$name" "$stage/payload/$name"
done
cp -- "$public" "$stage/payload/RPM-PUBLIC-KEY.asc"

for name in "$source_rpm" "$binary"; do
    if [[ "$name" == "$binary" ]]; then
        expected='lto-ltfs-0.1.1-22.el9.x86_64'
    else
        expected='lto-ltfs-0.1.1-22.el9.src'
    fi
    observed=$(rpm -qp --qf '%{NAME}-%{VERSION}-%{RELEASE}.%{ARCH}' "$stage/payload/$name")
    if [[ "$observed" != "$expected" ]]; then
        printf 'driver RPM NEVRA differs from approved release\n' >&2
        exit 1
    fi
    GNUPGHOME="$stage/gnupg" rpmsign \
        --define "_gpg_name ${subkey}!" \
        --define "_gpg_path $stage/gnupg" \
        --define '__gpg /usr/bin/gpg' \
        --addsign "$stage/payload/$name"
    signature=$(rpmkeys --dbpath "$stage/rpmdb" --checksig --verbose "$stage/payload/$name")
    suffix=${subkey: -8}
    if ! printf '%s\n' "$signature" | tr '[:upper:]' '[:lower:]' | \
        grep -Fq "key id ${suffix,,}: ok"; then
        printf 'driver RPM signature differs from approved signing subkey\n' >&2
        exit 1
    fi
    rpm --dbpath "$stage/rpmdb" -K "$stage/payload/$name"
done

for name in "$source_rpm" "$binary"; do
    digest=$(sha256sum "$stage/payload/$name" | cut -d' ' -f1)
    printf '%s  %s\n' "$digest" "$name" >> "$stage/payload/FINAL-RPM-SHA256SUMS"
done
gpg --homedir "$stage/gnupg" --batch --yes --armor --detach-sign \
    --local-user "${subkey}!" \
    --output "$stage/payload/FINAL-RPM-SHA256SUMS.asc" \
    "$stage/payload/FINAL-RPM-SHA256SUMS"

# Private signing material and the transient RPM database never enter the candidate.
mv -- "$stage/payload" "$output"
printf 'signed driver candidate staged without publication: %s\n' "$output"
