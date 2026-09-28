/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_TEST_FAKE_FSOPS_RAW_H
#define LTO_LTFS_TEST_FAKE_FSOPS_RAW_H

#include "ltfs.h"

int ltfs_fsraw_open(const char *path, bool open_write, struct dentry **dentry,
	struct ltfs_volume *volume);
int ltfs_fsraw_close(struct dentry *dentry);
ssize_t ltfs_fsraw_read(struct dentry *dentry, char *buffer, size_t size,
	off_t offset, struct ltfs_volume *volume);
ssize_t ltfs_fsraw_write(struct dentry *dentry, const char *buffer, size_t size,
	off_t offset, int partition, bool use_scheduler,
	struct ltfs_volume *volume);
int ltfs_fsraw_truncate(struct dentry *dentry, off_t length,
	struct ltfs_volume *volume);

#endif
