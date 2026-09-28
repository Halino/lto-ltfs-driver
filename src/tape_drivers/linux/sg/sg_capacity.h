/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_SG_CAPACITY_H
#define LTO_LTFS_SG_CAPACITY_H

#include "libltfs/tape_ops.h"

#include <stddef.h>

bool sg_capacity_uses_page31(int vendor, int drive_type);

/* Page31 capacity values and the partition1 offset are expressed in MiB.
 * The output is cleared on failure whenever capacity is non-NULL. */
int sg_parse_capacity_page31(const unsigned char *page, size_t size,
	unsigned int capacity_offset, struct tc_remaining_cap *capacity);

int sg_parse_capacity_page17(const unsigned char *page, size_t size,
	unsigned int capacity_offset, struct tc_remaining_cap *capacity);

#endif /* LTO_LTFS_SG_CAPACITY_H */
