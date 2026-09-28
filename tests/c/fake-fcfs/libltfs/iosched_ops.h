/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_TEST_FAKE_IOSCHED_OPS_H
#define LTO_LTFS_TEST_FAKE_IOSCHED_OPS_H

#include "ltfs.h"

struct iosched_ops {
	void *(*init)(struct ltfs_volume *volume);
	int (*destroy)(void *handle);
	int (*open)(const char *path, bool write, struct dentry **dentry,
		void *handle);
	int (*close)(struct dentry *dentry, bool flush, void *handle);
	ssize_t (*read)(struct dentry *dentry, char *buffer, size_t size,
		off_t offset, void *handle);
	ssize_t (*write)(struct dentry *dentry, const char *buffer, size_t size,
		off_t offset, bool update_time, void *handle);
	int (*flush)(struct dentry *dentry, bool close, void *handle);
	int (*truncate)(struct dentry *dentry, off_t length, void *handle);
	uint64_t (*get_filesize)(struct dentry *dentry, void *handle);
	int (*update_data_placement)(struct dentry *dentry, void *handle);
	int (*set_profiler)(char *work_directory, bool enable, void *handle);
	int (*get_queue_fill)(uint64_t *bytes, void *handle);
	int (*get_buffer_underrun_count)(uint64_t *count, void *handle);
};

#endif
