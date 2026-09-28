/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_DEVICE_IDENTITY_H
#define LTO_LTFS_DEVICE_IDENTITY_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

#define LTFS_DEVICE_IDENTITY_TEXT_MAX 256
#define LTFS_DEVICE_BOOT_ID_MAX 37
#define LTFS_DEVICE_SHA256_HEX_SIZE 65
#define LTFS_DEVICE_CONFIG_PATH "/etc/lto-ltfs/device.json"
#define LTFS_DEVICE_LOCK_ROOT "/run/lock/lto-ltfs"

struct ltfs_device_config {
	char nst_path[PATH_MAX];
	char sg_path[PATH_MAX];
	char expected_serial[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char expected_wwid[LTFS_DEVICE_IDENTITY_TEXT_MAX];
};

struct ltfs_device_target {
	const char *nst_path;
	const char *sg_path;
	const char *expected_serial;
	const char *expected_wwid;
	const char *device_root;
	const char *sysfs_root;
	const char *proc_root;
	const char *lock_root;
};

struct ltfs_device_identity {
	char nst_path[PATH_MAX];
	char sg_path[PATH_MAX];
	char serial[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char wwid[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char scsi_tuple[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char target_sha256[LTFS_DEVICE_SHA256_HEX_SIZE];
	char boot_id[LTFS_DEVICE_BOOT_ID_MAX];
	char proc_root[PATH_MAX];
	char lock_root[PATH_MAX];
	dev_t nst_rdev;
	dev_t sg_rdev;
};

enum ltfs_device_identity_phase {
	LTFS_DEVICE_IDENTITY_PHASE_ARGUMENTS = 0,
	LTFS_DEVICE_IDENTITY_PHASE_NST_OPEN_FIRST,
	LTFS_DEVICE_IDENTITY_PHASE_SG_OPEN_FIRST,
	LTFS_DEVICE_IDENTITY_PHASE_DEVICE_ROOT,
	LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_CLASS,
	LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_SUBSYSTEM,
	LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_DEVICE,
	LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_SERIAL,
	LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_WWID,
	LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_CLASS,
	LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_SUBSYSTEM,
	LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_DEVICE,
	LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_SERIAL,
	LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_WWID,
	LTFS_DEVICE_IDENTITY_PHASE_NST_CLASS,
	LTFS_DEVICE_IDENTITY_PHASE_SG_CLASS,
	LTFS_DEVICE_IDENTITY_PHASE_NST_NAME,
	LTFS_DEVICE_IDENTITY_PHASE_SG_NAME,
	LTFS_DEVICE_IDENTITY_PHASE_NST_TUPLE,
	LTFS_DEVICE_IDENTITY_PHASE_SCSI_TUPLE,
	LTFS_DEVICE_IDENTITY_PHASE_DRIVE_SERIAL,
	LTFS_DEVICE_IDENTITY_PHASE_DRIVE_WWID,
	LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_SERIAL,
	LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_WWID,
	LTFS_DEVICE_IDENTITY_PHASE_BOOT_ID,
	LTFS_DEVICE_IDENTITY_PHASE_NST_OPEN_SECOND,
	LTFS_DEVICE_IDENTITY_PHASE_SG_OPEN_SECOND,
	LTFS_DEVICE_IDENTITY_PHASE_DEVICE_STABILITY,
	LTFS_DEVICE_IDENTITY_PHASE_COPY_RESULT,
	LTFS_DEVICE_IDENTITY_PHASE_HASH,
	LTFS_DEVICE_IDENTITY_PHASE_COMPLETE
};

struct ltfs_device_identity_diagnostic {
	enum ltfs_device_identity_phase phase;
	int error;
};

static inline bool ltfs_device_config_metadata_is_trusted(mode_t mode, uid_t uid)
{
	mode_t permissions = mode & 07777;
	return S_ISREG(mode) && uid == 0 &&
		(permissions == 0600 || permissions == 0640);
}

int ltfs_device_identity_resolve(const struct ltfs_device_target *target,
	struct ltfs_device_identity *identity);
int ltfs_device_identity_resolve_diagnostic(
	const struct ltfs_device_target *target,
	struct ltfs_device_identity *identity,
	struct ltfs_device_identity_diagnostic *diagnostic);
const char *ltfs_device_identity_phase_name(
	enum ltfs_device_identity_phase phase);
int ltfs_device_config_parse(const char *json, size_t length,
	struct ltfs_device_config *config);
int ltfs_device_config_load(const char *path,
	struct ltfs_device_config *config);
int ltfs_device_identity_normalize_text(char *destination,
	size_t destination_size, const unsigned char *source,
	size_t source_size);

#endif /* LTO_LTFS_DEVICE_IDENTITY_H */
