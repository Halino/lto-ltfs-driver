#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause

set -euo pipefail

project_name=lto-ltfs
project_version=0.1.1
output_directory=
source_only=0
rpm_bundle=
rpm_lock=

usage() {
    printf 'usage: %s [--source-only] [--rpm-bundle DIR --rpm-lock FILE] --output DIRECTORY\n' "$0"
}

while (($#)); do
    case "$1" in
        --output)
            if (($# < 2)) || [[ -z "$2" ]]; then
                usage >&2
                exit 2
            fi
            output_directory=$2
            shift 2
            ;;
        --source-only)
            source_only=1
            shift
            ;;
        --rpm-bundle)
            if (($# < 2)) || [[ -z "$2" ]]; then
                usage >&2
                exit 2
            fi
            rpm_bundle=$2
            shift 2
            ;;
        --rpm-lock)
            if (($# < 2)) || [[ -z "$2" ]]; then
                usage >&2
                exit 2
            fi
            rpm_lock=$2
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$output_directory" ]] || [[ -e "$output_directory" ]]; then
    printf 'output directory must be a new path\n' >&2
    exit 2
fi

for required_tool in git gzip sha256sum mktemp python3; do
    command -v "$required_tool" >/dev/null || {
        printf 'required build tool is unavailable: %s\n' "$required_tool" >&2
        exit 1
    }
done

script_directory=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repository_root=$(git -C "$script_directory" rev-parse --show-toplevel)
if [[ -n "$(git -C "$repository_root" status --porcelain --untracked-files=all)" ]]; then
    printf 'RPM builds require a clean Git worktree\n' >&2
    exit 1
fi

source_date_epoch=$(git -C "$repository_root" show -s --format=%ct HEAD)
if [[ ! "$source_date_epoch" =~ ^[0-9]+$ ]]; then
    printf 'Git commit has an invalid source date\n' >&2
    exit 1
fi

temporary_root=$(mktemp -d "${TMPDIR:-/tmp}/lto-ltfs-rpm.XXXXXXXX")
cleanup() {
    rm -rf -- "$temporary_root"
}
trap cleanup EXIT HUP INT TERM

staging_directory=$temporary_root/output
mkdir -m0755 "$staging_directory"
source_archive=$staging_directory/$project_name-$project_version.tar.gz
git -C "$repository_root" archive \
    --format=tar \
    --prefix="$project_name-$project_version/" \
    HEAD | gzip -n > "$source_archive"
source_manifest=$staging_directory/SOURCE-MANIFEST.json
python3 "$repository_root/scripts/verify-rpm.py" \
    --write-source-manifest "$source_archive" "$source_manifest"

if ((source_only)); then
    (
        cd "$staging_directory"
        LC_ALL=C sha256sum "$project_name-$project_version.tar.gz" \
            SOURCE-MANIFEST.json > SHA256SUMS
    )
    mv -- "$staging_directory" "$output_directory"
    printf 'source archive written to %s\n' "$output_directory"
    exit 0
fi

if [[ -z "$rpm_bundle" || -z "$rpm_lock" ]]; then
    printf 'full RPM builds require --rpm-bundle and --rpm-lock\n' >&2
    exit 1
fi
if [[ ! -d "$rpm_bundle" || -L "$rpm_bundle" \
    || ! -f "$rpm_lock" || -L "$rpm_lock" ]]; then
    printf 'RPM bundle or lock is not a regular trusted input\n' >&2
    exit 1
fi
rpm_bundle=$(cd -- "$rpm_bundle" && pwd -P)
rpm_lock_directory=$(cd -- "$(dirname -- "$rpm_lock")" && pwd -P)
rpm_lock=$rpm_lock_directory/$(basename -- "$rpm_lock")
if [[ ! -d "$rpm_bundle/build" || ! -d "$rpm_bundle/runtime" \
    || -L "$rpm_bundle/build" || -L "$rpm_bundle/runtime" ]]; then
    printf 'RPM bundle must contain build and runtime directories\n' >&2
    exit 1
fi
mapfile -t bundle_entries < <(
    find "$rpm_bundle" -mindepth 1 -maxdepth 2 -printf '%P\n' | LC_ALL=C sort
)
mapfile -t bundle_rpms < <(
    find "$rpm_bundle" -mindepth 2 -maxdepth 2 -type f -name '*.rpm' \
        -printf '%P\n' | LC_ALL=C sort
)
if ((${#bundle_rpms[@]} < 2 \
    || ${#bundle_entries[@]} != ${#bundle_rpms[@]} + 2)); then
    printf 'RPM bundle contains non-RPM, nested, or missing entries\n' >&2
    exit 1
fi
mapfile -t build_rpms < <(find "$rpm_bundle/build" -maxdepth 1 -type f -name '*.rpm' -print)
mapfile -t runtime_rpms < <(find "$rpm_bundle/runtime" -maxdepth 1 -type f -name '*.rpm' -print)
if ((${#build_rpms[@]} == 0 || ${#runtime_rpms[@]} == 0)); then
    printf 'RPM bundle build and runtime sets must both be non-empty\n' >&2
    exit 1
fi
locked_rpms=()
while IFS= read -r lock_line; do
    if [[ "$lock_line" =~ ^[0-9a-f]{64}[[:space:]][[:space:]]((build|runtime)/[A-Za-z0-9._+-]+\.rpm)$ ]]; then
        locked_rpms+=("${BASH_REMATCH[1]}")
    else
        printf 'RPM lock contains an invalid entry\n' >&2
        exit 1
    fi
done < "$rpm_lock"
mapfile -t locked_rpms < <(printf '%s\n' "${locked_rpms[@]}" | LC_ALL=C sort)
if [[ "${locked_rpms[*]}" != "${bundle_rpms[*]}" ]]; then
    printf 'RPM bundle differs from its exact lock manifest\n' >&2
    exit 1
fi
(cd "$rpm_bundle" && LC_ALL=C sha256sum -c "$rpm_lock")

for required_tool in podman cmp find sort; do
    command -v "$required_tool" >/dev/null || {
        printf 'required container build tool is unavailable: %s\n' "$required_tool" >&2
        exit 1
    }
done

context_directory=$temporary_root/context
mkdir -m0755 "$context_directory"
cp -- "$source_archive" "$context_directory/"
cp -- "$source_manifest" "$context_directory/"
cp -- "$repository_root/packaging/udev/99-lto-ltfs.rules" "$context_directory/"
cp -- "$repository_root/packaging/tmpfiles/lto-ltfs.conf" "$context_directory/"
cp -- "$repository_root/packaging/rpm/lto-ltfs.spec" "$context_directory/"
cp -- "$repository_root/packaging/rpm/Containerfile" "$context_directory/"
cp -- "$repository_root/scripts/verify-rpm.py" "$context_directory/"
cp -R -- "$rpm_bundle" "$context_directory/rpm-bundle"
cp -- "$rpm_lock" "$context_directory/RPM-BUNDLE.sha256"

for build_number in 1 2; do
    build_output=$temporary_root/container-output-$build_number
    podman build \
        --no-cache \
        --pull=never \
        --network=none \
        --file "$context_directory/Containerfile" \
        --build-arg "SOURCE_DATE_EPOCH=$source_date_epoch" \
        --output "type=local,dest=$build_output" \
        "$context_directory"
done

first_output=$temporary_root/container-output-1
second_output=$temporary_root/container-output-2
for directory in "$first_output" "$second_output"; do
    mapfile -t entries < <(find "$directory" -mindepth 1 -maxdepth 1 -printf '%f\n' | LC_ALL=C sort)
    mapfile -t source_rpms < <(find "$directory" -maxdepth 1 -type f -name '*.src.rpm' -printf '%f\n')
    mapfile -t binary_rpms < <(find "$directory" -maxdepth 1 -type f -name '*.x86_64.rpm' -printf '%f\n')
    if ((${#entries[@]} != 6 || ${#source_rpms[@]} != 1 || ${#binary_rpms[@]} != 1)) \
        || [[ ! -f "$directory/$project_name-$project_version.tar.gz" \
            || ! -f "$directory/RPM-PAYLOAD-DIGEST" \
            || ! -f "$directory/SOURCE-MANIFEST.json" \
            || ! -f "$directory/SHA256SUMS" ]]; then
        printf 'container build output is outside the exact artifact allowlist\n' >&2
        exit 1
    fi
    (cd "$directory" && LC_ALL=C sha256sum -c SHA256SUMS)
    cmp --silent "$source_archive" \
        "$directory/$project_name-$project_version.tar.gz" || {
        printf 'container source archive differs from the trusted Git archive\n' >&2
        exit 1
    }
    cmp --silent "$source_manifest" "$directory/SOURCE-MANIFEST.json" || {
        printf 'container source manifest differs from the trusted host manifest\n' >&2
        exit 1
    }
done

mapfile -t first_entries < <(find "$first_output" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
mapfile -t second_entries < <(find "$second_output" -mindepth 1 -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort)
if [[ "${first_entries[*]}" != "${second_entries[*]}" ]]; then
    printf 'container builds produced different artifact sets\n' >&2
    exit 1
fi
for artifact in "${first_entries[@]}"; do
    if ! cmp --silent "$first_output/$artifact" "$second_output/$artifact"; then
        printf 'container builds are not byte reproducible: %s\n' "$artifact" >&2
        exit 1
    fi
done

rm -rf -- "$staging_directory"
mv -- "$first_output" "$output_directory"
printf 'verified RPM artifacts written to %s\n' "$output_directory"
