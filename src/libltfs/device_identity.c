/* SPDX-License-Identifier: LGPL-2.1-only */
/* Downstream modifications: 2026-08 through 2026-09; see LGPL-NOTICE. */

#include "device_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef O_PATH
#define O_PATH 010000000
#endif

#define MAX_SYMLINK_DEPTH 16

int ltfs_device_identity_normalize_text(char *destination,
	size_t destination_size, const unsigned char *source,
	size_t source_size)
{
	size_t first = 0;
	size_t last = source_size;
	size_t index;

	if (!destination || destination_size == 0 || !source || source_size == 0)
		return -EINVAL;
	while (first < last && source[first] == ' ')
		++first;
	while (last > first &&
	       (source[last - 1] == ' ' || source[last - 1] == '\0'))
		--last;
	if (first == last || last - first >= destination_size)
		return -EINVAL;
	for (index = first; index < last; ++index)
		if (source[index] < 0x20 || source[index] > 0x7e)
			return -EINVAL;
	memcpy(destination, source + first, last - first);
	destination[last - first] = '\0';
	return 0;
}
#define DEVICE_CONFIG_MAX 4096

struct opened_device {
	struct stat status;
	char canonical_path[PATH_MAX];
};

struct sysfs_identity {
	char class_name[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char device_name[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char tuple[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char serial[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	char wwid[LTFS_DEVICE_IDENTITY_TEXT_MAX];
};

static int diagnostic_result(struct ltfs_device_identity_diagnostic *diagnostic,
	enum ltfs_device_identity_phase phase, int result)
{
	if (diagnostic) {
		diagnostic->phase = phase;
		diagnostic->error = result < 0 ? -result : 0;
	}
	return result;
}

const char *ltfs_device_identity_phase_name(
	enum ltfs_device_identity_phase phase)
{
	switch (phase) {
	case LTFS_DEVICE_IDENTITY_PHASE_ARGUMENTS: return "arguments";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_OPEN_FIRST: return "nst-open-first";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_OPEN_FIRST: return "sg-open-first";
	case LTFS_DEVICE_IDENTITY_PHASE_DEVICE_ROOT: return "device-root";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_CLASS: return "nst-sysfs-class";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_SUBSYSTEM: return "nst-sysfs-subsystem";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_DEVICE: return "nst-sysfs-device";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_SERIAL: return "nst-sysfs-serial";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_WWID: return "nst-sysfs-wwid";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_CLASS: return "sg-sysfs-class";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_SUBSYSTEM: return "sg-sysfs-subsystem";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_DEVICE: return "sg-sysfs-device";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_SERIAL: return "sg-sysfs-serial";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_WWID: return "sg-sysfs-wwid";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_CLASS: return "nst-class";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_CLASS: return "sg-class";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_NAME: return "nst-name";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_NAME: return "sg-name";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_TUPLE: return "nst-tuple";
	case LTFS_DEVICE_IDENTITY_PHASE_SCSI_TUPLE: return "scsi-tuple";
	case LTFS_DEVICE_IDENTITY_PHASE_DRIVE_SERIAL: return "drive-serial";
	case LTFS_DEVICE_IDENTITY_PHASE_DRIVE_WWID: return "drive-wwid";
	case LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_SERIAL: return "expected-serial";
	case LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_WWID: return "expected-wwid";
	case LTFS_DEVICE_IDENTITY_PHASE_BOOT_ID: return "boot-id";
	case LTFS_DEVICE_IDENTITY_PHASE_NST_OPEN_SECOND: return "nst-open-second";
	case LTFS_DEVICE_IDENTITY_PHASE_SG_OPEN_SECOND: return "sg-open-second";
	case LTFS_DEVICE_IDENTITY_PHASE_DEVICE_STABILITY: return "device-stability";
	case LTFS_DEVICE_IDENTITY_PHASE_COPY_RESULT: return "copy-result";
	case LTFS_DEVICE_IDENTITY_PHASE_HASH: return "hash";
	case LTFS_DEVICE_IDENTITY_PHASE_COMPLETE: return "complete";
	default: return "unknown";
	}
}

static int copy_text(char *destination, size_t size, const char *source)
{
	size_t length;
	if (!destination || !source || size == 0)
		return -EINVAL;
	length = strlen(source);
	if (length >= size)
		return -ENAMETOOLONG;
	memcpy(destination, source, length + 1);
	return 0;
}

static const char *path_basename(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int normalize_leaf_path(const char *path, char *normalized, size_t size)
{
	char copy[PATH_MAX];
	char parent[PATH_MAX];
	char resolved_parent[PATH_MAX];
	char *slash;
	const char *leaf;
	int length;

	if (!path || strlen(path) >= sizeof(copy))
		return -ENAMETOOLONG;
	strcpy(copy, path);
	slash = strrchr(copy, '/');
	if (slash) {
		leaf = slash + 1;
		if (slash == copy)
			strcpy(parent, "/");
		else {
			*slash = '\0';
			if (copy_text(parent, sizeof(parent), copy) < 0)
				return -ENAMETOOLONG;
		}
	} else {
		leaf = copy;
		if (!getcwd(parent, sizeof(parent)))
			return -errno;
	}
	if (!realpath(parent, resolved_parent))
		return -errno;
	length = snprintf(normalized, size, "%s%s%s", resolved_parent,
		strcmp(resolved_parent, "/") ? "/" : "", leaf);
	return length >= 0 && length < (int)size ? 0 : -ENAMETOOLONG;
}

static int next_symlink_path(const char *current, int fd, char *next,
	size_t size)
{
	char link[PATH_MAX];
	char combined[PATH_MAX];
	char parent[PATH_MAX];
	char *slash;
	ssize_t length = readlinkat(fd, "", link, sizeof(link) - 1);
	if (length < 0)
		return -errno;
	link[length] = '\0';
	if (link[0] == '/')
		return normalize_leaf_path(link, next, size);
	if (copy_text(parent, sizeof(parent), current) < 0)
		return -ENAMETOOLONG;
	slash = strrchr(parent, '/');
	if (!slash)
		strcpy(parent, ".");
	else if (slash == parent)
		parent[1] = '\0';
	else
		*slash = '\0';
	length = snprintf(combined, sizeof(combined), "%s/%s", parent, link);
	if (length < 0 || length >= (ssize_t)sizeof(combined))
		return -ENAMETOOLONG;
	return normalize_leaf_path(combined, next, size);
}

static bool device_name_matches(const char *name, const char *prefix)
{
	const char *cursor;
	if (strncmp(name, prefix, strlen(prefix)) != 0)
		return false;
	cursor = name + strlen(prefix);
	if (*cursor == '\0')
		return false;
	for (; *cursor; ++cursor)
		if (*cursor < '0' || *cursor > '9')
			return false;
	return true;
}

static int open_device_path(const char *path, const char *required_prefix,
	struct opened_device *opened)
{
	char current[PATH_MAX];
	char candidate[PATH_MAX] = "";
	char proc_fd[64];
	char proc_target[PATH_MAX];
	struct stat status;
	ssize_t proc_length;
	int depth;
	int fd = -1;
	int ret;

	ret = normalize_leaf_path(path, current, sizeof(current));
	if (ret < 0)
		return ret;
	for (depth = 0; depth < MAX_SYMLINK_DEPTH; ++depth) {
		fd = open(current, O_PATH | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0)
			return -errno;
		if (fstat(fd, &status) < 0) {
			ret = -errno;
			close(fd);
			return ret;
		}
		if (S_ISCHR(status.st_mode))
			break;
		if (!S_ISLNK(status.st_mode)) {
			close(fd);
			return -ENODEV;
		}
		if (device_name_matches(path_basename(current), required_prefix)) {
			ret = copy_text(candidate, sizeof(candidate), current);
			if (ret < 0) {
				close(fd);
				return ret;
			}
		}
		ret = next_symlink_path(current, fd, current, sizeof(current));
		close(fd);
		fd = -1;
		if (ret < 0)
			return ret;
	}
	if (fd < 0 || !S_ISCHR(status.st_mode)) {
		if (fd >= 0)
			close(fd);
		return -ELOOP;
	}
	if (candidate[0] == '\0' &&
		device_name_matches(path_basename(current), required_prefix))
		ret = copy_text(candidate, sizeof(candidate), current);
	else
		ret = candidate[0] ? 0 : -ENODEV;
	if (ret < 0) {
		close(fd);
		return ret;
	}
	/* Resolve the anchored descriptor as an independent check of the target. */
	snprintf(proc_fd, sizeof(proc_fd), "/proc/self/fd/%d", fd);
	proc_length = readlink(proc_fd, proc_target, sizeof(proc_target) - 1);
	if (proc_length < 0) {
		ret = -errno;
		close(fd);
		return ret;
	}
	proc_target[proc_length] = '\0';
	opened->status = status;
	ret = copy_text(opened->canonical_path,
		sizeof(opened->canonical_path), candidate);
	close(fd);
	return ret;
}

static int read_trimmed_file(const char *path, char *value, size_t size)
{
	char *buffer;
	int fd;
	ssize_t length;
	if (!path || !value || size < 2)
		return -EINVAL;
	buffer = malloc(size + 1);
	if (!buffer)
		return -ENOMEM;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		free(buffer);
		return -errno;
	}
	length = read(fd, buffer, size);
	if (length < 0) {
		int ret = -errno;
		close(fd);
		free(buffer);
		return ret;
	}
	if (close(fd) < 0) {
		free(buffer);
		return -errno;
	}
	if (length == 0) {
		free(buffer);
		return -EINVAL;
	}
	buffer[length] = '\0';
	while (length > 0 && (buffer[length - 1] == '\n' ||
		buffer[length - 1] == '\r' || buffer[length - 1] == ' ' ||
		buffer[length - 1] == '\t'))
		buffer[--length] = '\0';
	if (length == 0 || length >= (ssize_t)size) {
		free(buffer);
		return -EINVAL;
	}
	memcpy(value, buffer, (size_t)length + 1);
	free(buffer);
	return 0;
}

static int read_vpd80_serial(const char *path, char *serial, size_t size)
{
	unsigned char page[4 + LTFS_DEVICE_IDENTITY_TEXT_MAX];
	size_t used = 0;
	size_t payload_length;
	size_t first;
	size_t last;
	int fd;
	if (!path || !serial || size < 2)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	while (used < sizeof(page)) {
		ssize_t length = read(fd, page + used, sizeof(page) - used);
		if (length < 0 && errno == EINTR)
			continue;
		if (length < 0) {
			int ret = -errno;
			close(fd);
			return ret;
		}
		if (length == 0)
			break;
		used += (size_t)length;
	}
	if (close(fd) < 0)
		return -errno;
	if (used < 5 || used == sizeof(page) || page[0] != 0x01 ||
		page[1] != 0x80)
		return -EINVAL;
	payload_length = ((size_t)page[2] << 8) | page[3];
	if (payload_length == 0 || payload_length >= size ||
		payload_length + 4 != used)
		return -EINVAL;
	first = 4;
	last = used;
	while (first < last && page[first] == ' ')
		++first;
	while (last > first && page[last - 1] == ' ')
		--last;
	if (first == last || last - first >= size)
		return -EINVAL;
	for (used = first; used < last; ++used)
		if (page[used] < 0x20 || page[used] > 0x7e)
			return -EINVAL;
	memcpy(serial, page + first, last - first);
	serial[last - first] = '\0';
	return 0;
}

static bool valid_wwid(const char *wwid)
{
	const char *cursor;
	bool hexadecimal;
	if (!wwid || (strncmp(wwid, "naa.", 4) &&
		strncmp(wwid, "eui.", 4) && strncmp(wwid, "t10.", 4)))
		return false;
	if (!wwid[4])
		return false;
	hexadecimal = !strncmp(wwid, "naa.", 4) || !strncmp(wwid, "eui.", 4);
	for (cursor = wwid + 4; *cursor; ++cursor) {
		unsigned char byte = (unsigned char)*cursor;
		if (byte < 0x21 || byte > 0x7e)
			return false;
		if (hexadecimal && !((byte >= '0' && byte <= '9') ||
			(byte >= 'a' && byte <= 'f') ||
			(byte >= 'A' && byte <= 'F')))
			return false;
	}
	return true;
}

static int read_sysfs_identity(const char *sysfs_root, dev_t device,
	struct sysfs_identity *identity,
	struct ltfs_device_identity_diagnostic *diagnostic,
	enum ltfs_device_identity_phase class_phase)
{
	char directory[PATH_MAX];
	char path[PATH_MAX];
	int length = snprintf(directory, sizeof(directory), "%s/dev/char/%u:%u",
		sysfs_root, major(device), minor(device));
	if (length < 0 || length >= (int)sizeof(directory))
		return -ENAMETOOLONG;
	/* Real sysfs exposes dev/char as a class-device symlink. */
	{
		char class_path[PATH_MAX];
		char device_path[PATH_MAX];
		char subsystem_path[PATH_MAX];
		char subsystem[PATH_MAX];
		ssize_t subsystem_length;
		if (!realpath(directory, class_path))
			return diagnostic_result(diagnostic, class_phase, -errno);
		if (copy_text(identity->device_name,
			sizeof(identity->device_name), path_basename(class_path)) < 0)
			return diagnostic_result(diagnostic, class_phase,
				-ENAMETOOLONG);
		length = snprintf(subsystem_path, sizeof(subsystem_path),
			"%s/subsystem", class_path);
		if (length < 0 || length >= (int)sizeof(subsystem_path))
			return diagnostic_result(diagnostic, class_phase + 1,
				-ENAMETOOLONG);
		subsystem_length = readlink(subsystem_path, subsystem,
			sizeof(subsystem) - 1);
		if (subsystem_length < 0)
			return diagnostic_result(diagnostic, class_phase + 1, -errno);
		subsystem[subsystem_length] = '\0';
		if (copy_text(identity->class_name, sizeof(identity->class_name),
			path_basename(subsystem)) < 0)
			return diagnostic_result(diagnostic, class_phase + 1,
				-ENAMETOOLONG);
		length = snprintf(path, sizeof(path), "%s/device", class_path);
		if (length < 0 || length >= (int)sizeof(path))
			return diagnostic_result(diagnostic, class_phase + 2,
				-ENAMETOOLONG);
		if (!realpath(path, device_path))
			return diagnostic_result(diagnostic, class_phase + 2, -errno);
		if (copy_text(identity->tuple, sizeof(identity->tuple),
			path_basename(device_path)) < 0)
			return diagnostic_result(diagnostic, class_phase + 2,
				-ENAMETOOLONG);
		length = snprintf(path, sizeof(path), "%s/vpd_pg80", device_path);
		if (length < 0 || length >= (int)sizeof(path))
			return diagnostic_result(diagnostic, class_phase + 3,
				-ENAMETOOLONG);
		length = read_vpd80_serial(path, identity->serial,
			sizeof(identity->serial));
		if (length < 0)
			return diagnostic_result(diagnostic, class_phase + 3, length);
		length = snprintf(path, sizeof(path), "%s/wwid", device_path);
		if (length < 0 || length >= (int)sizeof(path))
			return diagnostic_result(diagnostic, class_phase + 4,
				-ENAMETOOLONG);
		length = read_trimmed_file(path, identity->wwid,
			sizeof(identity->wwid));
		if (length < 0)
			return diagnostic_result(diagnostic, class_phase + 4, length);
		return valid_wwid(identity->wwid) ? 0 :
			diagnostic_result(diagnostic, class_phase + 4, -EINVAL);
	}
}

static bool valid_scsi_tuple(const char *tuple)
{
	unsigned int separators = 0;
	const char *cursor;
	if (!tuple || !*tuple)
		return false;
	for (cursor = tuple; *cursor; ++cursor) {
		if (*cursor == ':') {
			if (cursor == tuple || cursor[-1] == ':' || separators == 3)
				return false;
			++separators;
		} else if (*cursor < '0' || *cursor > '9')
			return false;
	}
	return separators == 3 && cursor[-1] != ':';
}

static bool valid_boot_id(const char *value)
{
	size_t i;
	if (strlen(value) != 36)
		return false;
	for (i = 0; i < 36; ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (value[i] != '-')
				return false;
		} else if (!((value[i] >= '0' && value[i] <= '9') ||
			(value[i] >= 'a' && value[i] <= 'f') ||
			(value[i] >= 'A' && value[i] <= 'F')))
			return false;
	}
	return true;
}

static int hash_identity(struct ltfs_device_identity *identity)
{
	const char *fields[] = { identity->nst_path, identity->sg_path,
		identity->serial, identity->wwid, identity->scsi_tuple };
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int digest_length = 0;
	unsigned char *input;
	unsigned char *cursor;
	size_t total = 0;
	size_t i;
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
		total += strlen(fields[i]) + 1;
	input = malloc(total);
	if (!input)
		return -ENOMEM;
	cursor = input;
	for (i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		size_t length = strlen(fields[i]) + 1;
		memcpy(cursor, fields[i], length);
		cursor += length;
	}
	if (EVP_Digest(input, total, digest, &digest_length, EVP_sha256(), NULL) != 1 ||
		digest_length != 32) {
		free(input);
		return -EIO;
	}
	free(input);
	for (i = 0; i < digest_length; ++i)
		snprintf(identity->target_sha256 + i * 2, 3, "%02x", digest[i]);
	identity->target_sha256[64] = '\0';
	return 0;
}

static bool same_opened_device(const struct opened_device *left,
	const struct opened_device *right)
{
	return left->status.st_dev == right->status.st_dev &&
		left->status.st_ino == right->status.st_ino &&
		left->status.st_rdev == right->status.st_rdev &&
		strcmp(left->canonical_path, right->canonical_path) == 0;
}

static bool path_below_root(const char *path, const char *root)
{
	char resolved_root[PATH_MAX];
	size_t length;
	if (!path || !root || !realpath(root, resolved_root))
		return false;
	length = strlen(resolved_root);
	if (length == 1 && resolved_root[0] == '/')
		return path[0] == '/';
	return !strncmp(path, resolved_root, length) && path[length] == '/';
}

static void skip_json_space(const char **cursor, const char *end)
{
	while (*cursor < end && (**cursor == ' ' || **cursor == '\t' ||
		**cursor == '\r' || **cursor == '\n'))
		++*cursor;
}

static bool stable_device_path(const char *path, const char *prefix)
{
	const char *name;
	if (!path || !prefix || strncmp(path, prefix, strlen(prefix)))
		return false;
	name = path + strlen(prefix);
	return *name && !strchr(name, '/');
}

static int parse_json_string(const char **cursor, const char *end,
	char *value, size_t size)
{
	size_t length = 0;
	if (*cursor >= end || **cursor != '"')
		return -EINVAL;
	++*cursor;
	while (*cursor < end && **cursor != '"') {
		unsigned char byte = (unsigned char)**cursor;
		if (byte < 0x20 || byte == '\\' || length + 1 >= size)
			return -EINVAL;
		value[length++] = **cursor;
		++*cursor;
	}
	if (*cursor >= end || **cursor != '"' || length == 0)
		return -EINVAL;
	++*cursor;
	value[length] = '\0';
	return 0;
}

int ltfs_device_config_parse(const char *json, size_t length,
	struct ltfs_device_config *config)
{
	const char *cursor = json;
	const char *end;
	unsigned int fields = 0;
	if (!json || !config || length == 0 || length > DEVICE_CONFIG_MAX)
		return -EINVAL;
	end = json + length;
	memset(config, 0, sizeof(*config));
	skip_json_space(&cursor, end);
	if (cursor >= end || *cursor++ != '{')
		return -EINVAL;
	for (;;) {
		char key[32];
		char value[PATH_MAX];
		unsigned int bit;
		char *destination;
		size_t destination_size;
		skip_json_space(&cursor, end);
		if (cursor < end && *cursor == '}') {
			++cursor;
			break;
		}
		if (parse_json_string(&cursor, end, key, sizeof(key)) < 0)
			return -EINVAL;
		skip_json_space(&cursor, end);
		if (cursor >= end || *cursor++ != ':')
			return -EINVAL;
		skip_json_space(&cursor, end);
		if (parse_json_string(&cursor, end, value, sizeof(value)) < 0)
			return -EINVAL;
		if (!strcmp(key, "nst_path")) {
			bit = 1U;
			destination = config->nst_path;
			destination_size = sizeof(config->nst_path);
		} else if (!strcmp(key, "sg_path")) {
			bit = 2U;
			destination = config->sg_path;
			destination_size = sizeof(config->sg_path);
		} else if (!strcmp(key, "serial")) {
			bit = 4U;
			destination = config->expected_serial;
			destination_size = sizeof(config->expected_serial);
		} else if (!strcmp(key, "wwid")) {
			bit = 8U;
			destination = config->expected_wwid;
			destination_size = sizeof(config->expected_wwid);
		} else
			return -EINVAL;
		if ((fields & bit) || copy_text(destination, destination_size, value) < 0)
			return -EINVAL;
		fields |= bit;
		skip_json_space(&cursor, end);
		if (cursor < end && *cursor == ',') {
			++cursor;
			continue;
		}
		if (cursor < end && *cursor == '}') {
			++cursor;
			break;
		}
		return -EINVAL;
	}
	skip_json_space(&cursor, end);
	if (cursor != end || fields != 15U ||
		!stable_device_path(config->nst_path, "/dev/tape/by-id/") ||
		!stable_device_path(config->sg_path, "/dev/lto-archiver-scsi-"))
		return -EINVAL;
	return 0;
}

int ltfs_device_config_load(const char *path,
	struct ltfs_device_config *config)
{
	char buffer[DEVICE_CONFIG_MAX + 1];
	struct stat status;
	size_t used = 0;
	int fd;
	if (!path || !config)
		return -EINVAL;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &status) < 0) {
		int ret = -errno;
		close(fd);
		return ret;
	}
	if (!ltfs_device_config_metadata_is_trusted(status.st_mode,
		status.st_uid)) {
		close(fd);
		return -EPERM;
	}
	for (;;) {
		ssize_t length;
		do {
			length = read(fd, buffer + used, sizeof(buffer) - used);
		} while (length < 0 && errno == EINTR);
		if (length < 0) {
			int ret = -errno;
			close(fd);
			return ret;
		}
		if (length == 0)
			break;
		used += (size_t)length;
		if (used == sizeof(buffer)) {
			close(fd);
			return -EFBIG;
		}
	}
	if (close(fd) < 0)
		return -errno;
	if (used == 0)
		return -EINVAL;
	buffer[used] = '\0';
	return ltfs_device_config_parse(buffer, used, config);
}

int ltfs_device_identity_resolve_diagnostic(
	const struct ltfs_device_target *target,
	struct ltfs_device_identity *identity,
	struct ltfs_device_identity_diagnostic *diagnostic)
{
	struct opened_device nst_first;
	struct opened_device sg_first;
	struct opened_device nst_second;
	struct opened_device sg_second;
	struct sysfs_identity nst_sysfs;
	struct sysfs_identity sg_sysfs;
	char boot_path[PATH_MAX];
	int ret;

	if (diagnostic) {
		diagnostic->phase = LTFS_DEVICE_IDENTITY_PHASE_ARGUMENTS;
		diagnostic->error = 0;
	}
	if (!target || !identity || !target->nst_path || !target->sg_path ||
		!target->device_root ||
		!target->sysfs_root || !target->proc_root || !target->lock_root)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_ARGUMENTS, -EINVAL);
	memset(identity, 0, sizeof(*identity));
	ret = open_device_path(target->nst_path, "nst", &nst_first);
	if (ret < 0)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_NST_OPEN_FIRST, ret);
	ret = open_device_path(target->sg_path, "sg", &sg_first);
	if (ret < 0)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_SG_OPEN_FIRST, ret);
	if (!path_below_root(nst_first.canonical_path, target->device_root) ||
		!path_below_root(sg_first.canonical_path, target->device_root))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_DEVICE_ROOT, -EXDEV);
	ret = read_sysfs_identity(target->sysfs_root, nst_first.status.st_rdev,
		&nst_sysfs, diagnostic,
		LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_CLASS);
	if (ret < 0)
		return ret;
	ret = read_sysfs_identity(target->sysfs_root, sg_first.status.st_rdev,
		&sg_sysfs, diagnostic,
		LTFS_DEVICE_IDENTITY_PHASE_SG_SYSFS_CLASS);
	if (ret < 0)
		return ret;
	if (strcmp(nst_sysfs.class_name, "scsi_tape"))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_NST_CLASS, -EXDEV);
	if (strcmp(sg_sysfs.class_name, "scsi_generic"))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_SG_CLASS, -EXDEV);
	if (!device_name_matches(nst_sysfs.device_name, "nst"))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_NST_NAME, -EXDEV);
	if (!device_name_matches(sg_sysfs.device_name, "sg"))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_SG_NAME, -EXDEV);
	if (!valid_scsi_tuple(nst_sysfs.tuple))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_NST_TUPLE, -EXDEV);
	if (strcmp(nst_sysfs.tuple, sg_sysfs.tuple))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_SCSI_TUPLE, -EXDEV);
	if (strcmp(nst_sysfs.serial, sg_sysfs.serial))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_DRIVE_SERIAL, -EXDEV);
	if (strcmp(nst_sysfs.wwid, sg_sysfs.wwid))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_DRIVE_WWID, -EXDEV);
	if (target->expected_serial &&
		strcmp(target->expected_serial, nst_sysfs.serial))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_SERIAL, -EXDEV);
	if (target->expected_wwid &&
		strcmp(target->expected_wwid, nst_sysfs.wwid))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_WWID, -EXDEV);
	ret = snprintf(boot_path, sizeof(boot_path),
		"%s/sys/kernel/random/boot_id", target->proc_root);
	if (ret < 0 || ret >= (int)sizeof(boot_path))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_BOOT_ID, -ENAMETOOLONG);
	ret = read_trimmed_file(boot_path, identity->boot_id,
		sizeof(identity->boot_id));
	if (ret < 0 || !valid_boot_id(identity->boot_id))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_BOOT_ID,
			ret < 0 ? ret : -EINVAL);
	ret = open_device_path(target->nst_path, "nst", &nst_second);
	if (ret < 0)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_NST_OPEN_SECOND, ret);
	ret = open_device_path(target->sg_path, "sg", &sg_second);
	if (ret < 0)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_SG_OPEN_SECOND, ret);
	if (!same_opened_device(&nst_first, &nst_second) ||
		!same_opened_device(&sg_first, &sg_second))
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_DEVICE_STABILITY, -ESTALE);
	if ((ret = copy_text(identity->nst_path, sizeof(identity->nst_path),
		nst_first.canonical_path)) < 0 ||
		(ret = copy_text(identity->sg_path, sizeof(identity->sg_path),
		sg_first.canonical_path)) < 0 ||
		(ret = copy_text(identity->serial, sizeof(identity->serial),
		nst_sysfs.serial)) < 0 ||
		(ret = copy_text(identity->wwid, sizeof(identity->wwid),
		nst_sysfs.wwid)) < 0 ||
		(ret = copy_text(identity->scsi_tuple, sizeof(identity->scsi_tuple),
		nst_sysfs.tuple)) < 0 ||
		(ret = copy_text(identity->proc_root, sizeof(identity->proc_root),
		target->proc_root)) < 0 ||
		(ret = copy_text(identity->lock_root, sizeof(identity->lock_root),
		target->lock_root)) < 0)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_COPY_RESULT, ret);
	identity->nst_rdev = nst_first.status.st_rdev;
	identity->sg_rdev = sg_first.status.st_rdev;
	ret = hash_identity(identity);
	if (ret < 0)
		return diagnostic_result(diagnostic,
			LTFS_DEVICE_IDENTITY_PHASE_HASH, ret);
	return diagnostic_result(diagnostic,
		LTFS_DEVICE_IDENTITY_PHASE_COMPLETE, 0);
}

int ltfs_device_identity_resolve(const struct ltfs_device_target *target,
	struct ltfs_device_identity *identity)
{
	return ltfs_device_identity_resolve_diagnostic(target, identity, NULL);
}
