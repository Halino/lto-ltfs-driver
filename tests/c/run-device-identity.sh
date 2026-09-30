#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# The chmod(0000) fixture must test EACCES even in a root rpmbuild container.
# Drop only DAC bypass for this child; do not change the build or product.
set -eu
binary=${LTFS_DEVICE_IDENTITY_BINARY:?missing native test binary}
if [ "$(id -u)" -eq 0 ]; then
    exec setpriv --bounding-set=-dac_override,-dac_read_search -- "$binary" "$@"
fi
exec "$binary" "$@"
