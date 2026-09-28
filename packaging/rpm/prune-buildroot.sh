#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    printf 'usage: prune-buildroot.sh ABSOLUTE_BUILDROOT\n' >&2
    exit 2
fi

buildroot=${1%/}
case "$buildroot" in
    /*) ;;
    *)
        printf 'buildroot must be an existing non-root absolute directory\n' >&2
        exit 2
        ;;
esac
if [ -z "$buildroot" ] || [ "$buildroot" = / ] || [ ! -d "$buildroot" ]; then
    printf 'buildroot must be an existing non-root absolute directory\n' >&2
    exit 2
fi

for relative_path in \
    usr/share/ltfs/ltfs \
    usr/share/snmp/LTFS-MIB.txt \
    usr/share/snmp/LtfsSnmpTrapDef.txt
do
    rm -f -- "$buildroot/$relative_path"
done

for relative_directory in usr/share/ltfs usr/share/snmp; do
    rmdir -- "$buildroot/$relative_directory" 2>/dev/null || true
done
