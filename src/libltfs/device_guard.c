/* SPDX-License-Identifier: BSD-3-Clause */

#include "device_guard.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int broker_fd_number(const char *path)
{
	static const char prefix[] = "/proc/self/fd/";
	const char *cursor;
	unsigned long value = 0;

	if (!path || strncmp(path, prefix, sizeof(prefix) - 1))
		return -EINVAL;
	cursor = path + sizeof(prefix) - 1;
	if (*cursor < '0' || *cursor > '9' ||
		(*cursor == '0' && cursor[1] != '\0'))
		return -EINVAL;
	for (; *cursor; ++cursor) {
		if (*cursor < '0' || *cursor > '9')
			return -EINVAL;
		value = value * 10 + (unsigned int)(*cursor - '0');
		if (value > INT_MAX)
			return -EINVAL;
	}
	return value >= 3 ? (int)value : -EINVAL;
}

static int anchor_broker_fd(const char *requested_path,
	const char *configured_path, struct ltfs_device_guard *guard)
{
	struct stat requested_status;
	struct stat configured_status;
	int source_fd = broker_fd_number(requested_path);
	int length;

	if (source_fd < 0)
		return source_fd;
	guard->anchored_device_fd = fcntl(source_fd, F_DUPFD_CLOEXEC, 3);
	if (guard->anchored_device_fd < 0)
		return -errno;
	if (fstat(guard->anchored_device_fd, &requested_status) < 0 ||
		stat(configured_path, &configured_status) < 0) {
		int ret = -errno;
		close(guard->anchored_device_fd);
		guard->anchored_device_fd = -1;
		return ret;
	}
	if (!S_ISCHR(requested_status.st_mode) ||
		!S_ISCHR(configured_status.st_mode) ||
		requested_status.st_dev != configured_status.st_dev ||
		requested_status.st_ino != configured_status.st_ino ||
		requested_status.st_rdev != configured_status.st_rdev ||
		requested_status.st_rdev != guard->identity.sg_rdev) {
		close(guard->anchored_device_fd);
		guard->anchored_device_fd = -1;
		return -EXDEV;
	}
	length = snprintf(guard->backend_device_path,
		sizeof(guard->backend_device_path), "/proc/self/fd/%d",
		guard->anchored_device_fd);
	if (length < 0 || length >= (int)sizeof(guard->backend_device_path)) {
		close(guard->anchored_device_fd);
		guard->anchored_device_fd = -1;
		return -ENAMETOOLONG;
	}
	return 0;
}

int ltfs_device_guard_requirement(const char *tape_backend_name,
	const char *requested_device)
{
	struct stat status;
	char resolved[PATH_MAX];
	if (!tape_backend_name || !tape_backend_name[0] ||
		!requested_device || !requested_device[0])
		return -EINVAL;
	if (strcmp(tape_backend_name, "file"))
		return 1;
	if (requested_device[0] != '/' || !strcmp(requested_device, "/dev") ||
		!strncmp(requested_device, "/dev/", 5))
		return -EINVAL;
	if (!realpath(requested_device, resolved) || strcmp(resolved, requested_device))
		return -EINVAL;
	if (lstat(requested_device, &status) < 0)
		return -EINVAL;
	if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid())
		return -EINVAL;
	if (status.st_mode & (S_IWGRP | S_IWOTH))
		return -EPERM;
	return 0;
}

int ltfs_device_guard_acquire(const struct ltfs_device_config *config,
	const char *requested_device_path,
	const struct ltfs_device_guard_roots *roots,
	const char *operation_id, enum ltfs_command_class command_class,
	struct ltfs_device_guard *guard)
{
	struct ltfs_device_target target;
	int ret;
	if (!config || !roots || !operation_id || !guard ||
		!config->nst_path[0] || !config->sg_path[0] ||
		!config->expected_serial[0] || !config->expected_wwid[0] ||
		!roots->device_root || !roots->sysfs_root || !roots->proc_root ||
		!roots->lock_root)
		return -EINVAL;
	memset(guard, 0, sizeof(*guard));
	guard->anchored_device_fd = -1;
	memset(&target, 0, sizeof(target));
	target.nst_path = config->nst_path;
	target.sg_path = config->sg_path;
	target.expected_serial = config->expected_serial;
	target.expected_wwid = config->expected_wwid;
	target.device_root = roots->device_root;
	target.sysfs_root = roots->sysfs_root;
	target.proc_root = roots->proc_root;
	target.lock_root = roots->lock_root;
	ret = ltfs_device_identity_resolve(&target, &guard->identity);
	if (ret < 0)
		return ret;
	if (requested_device_path && strcmp(requested_device_path, config->sg_path))
		ret = anchor_broker_fd(requested_device_path, config->sg_path, guard);
	else {
		ret = snprintf(guard->backend_device_path,
			sizeof(guard->backend_device_path), "%s", config->sg_path);
		ret = ret < 0 || ret >= (int)sizeof(guard->backend_device_path) ?
			-ENAMETOOLONG : 0;
	}
	if (ret < 0)
		return ret;
	ret = ltfs_operation_lock_acquire(&guard->identity, operation_id,
		command_class, &guard->lock);
	if (ret < 0 && guard->anchored_device_fd >= 0) {
		close(guard->anchored_device_fd);
		guard->anchored_device_fd = -1;
		guard->backend_device_path[0] = '\0';
	}
	return ret;
}

void ltfs_device_guard_release(struct ltfs_device_guard *guard)
{
	if (guard) {
		ltfs_operation_lock_release(&guard->lock);
		if (guard->anchored_device_fd >= 0) {
			close(guard->anchored_device_fd);
			guard->anchored_device_fd = -1;
		}
		guard->backend_device_path[0] = '\0';
	}
}

int ltfs_device_guard_release_preserving_result(
	struct ltfs_device_guard *guard, bool *acquired, int result)
{
	if (acquired && *acquired) {
		ltfs_device_guard_release(guard);
		*acquired = false;
	}
	return result;
}
