/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_OPERATION_LOCK_H
#define LTO_LTFS_OPERATION_LOCK_H

#include "device_identity.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define LTFS_OPERATION_LOCK_BUSY (-EWOULDBLOCK)
#define LTFS_OPERATION_LOCK_CORRUPT (-EBADMSG)

enum ltfs_command_class {
	LTFS_COMMAND_CLASS_MOUNT = 1,
	LTFS_COMMAND_CLASS_CHECK,
	LTFS_COMMAND_CLASS_DIAGNOSTIC,
	LTFS_COMMAND_CLASS_UNLOAD,
	LTFS_COMMAND_CLASS_FIRMWARE,
	LTFS_COMMAND_CLASS_READ_ONLY_INFO,
};

struct ltfs_operation_lock {
	bool owned;
	char path[PATH_MAX];
	char proc_root[PATH_MAX];
	char boot_id[LTFS_DEVICE_BOOT_ID_MAX];
	char operation_id[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char target_sha256[LTFS_DEVICE_SHA256_HEX_SIZE];
	enum ltfs_command_class command_class;
	pid_t pid;
	uint64_t start_ticks;
	uint64_t created_unix_ns;
	dev_t record_device;
	ino_t record_inode;
};

int ltfs_operation_lock_acquire(const struct ltfs_device_identity *identity,
	const char *operation_id, enum ltfs_command_class command_class,
	struct ltfs_operation_lock *lock);
void ltfs_operation_lock_release(struct ltfs_operation_lock *lock);

#endif /* LTO_LTFS_OPERATION_LOCK_H */
