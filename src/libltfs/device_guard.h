/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_DEVICE_GUARD_H
#define LTO_LTFS_DEVICE_GUARD_H

#include "device_identity.h"
#include "operation_lock.h"

struct ltfs_device_guard_roots {
	const char *device_root;
	const char *sysfs_root;
	const char *proc_root;
	const char *lock_root;
};

struct ltfs_device_guard {
	struct ltfs_device_identity identity;
	struct ltfs_operation_lock lock;
	int anchored_device_fd;
	char backend_device_path[PATH_MAX];
};

int ltfs_device_guard_requirement(const char *tape_backend_name,
	const char *requested_device);
int ltfs_device_guard_acquire(const struct ltfs_device_config *config,
	const char *requested_device_path,
	const struct ltfs_device_guard_roots *roots,
	const char *operation_id, enum ltfs_command_class command_class,
	struct ltfs_device_guard *guard);
void ltfs_device_guard_release(struct ltfs_device_guard *guard);
int ltfs_device_guard_release_preserving_result(
	struct ltfs_device_guard *guard, bool *acquired, int result);

#endif /* LTO_LTFS_DEVICE_GUARD_H */
