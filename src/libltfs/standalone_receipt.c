/* SPDX-License-Identifier: BSD-3-Clause */

#include "standalone_receipt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

static int write_all(int fd, const char *payload, size_t length)
{
	while (length) {
		ssize_t written = write(fd, payload, length);
		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (written == 0)
			return -EIO;
		payload += written;
		length -= (size_t)written;
	}
	return 0;
}

static bool valid_identifier(const char *value)
{
	unsigned int i;

	if (!value || strlen(value) != 36)
		return false;
	for (i = 0; i < 36; ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (value[i] != '-') return false;
		} else if (!((value[i] >= '0' && value[i] <= '9') ||
			(value[i] >= 'a' && value[i] <= 'f')))
			return false;
	}
	if (value[14] < '1' || value[14] > '5')
		return false;
	return value[19] == '8' || value[19] == '9' ||
		value[19] == 'a' || value[19] == 'b';
}

static int receipt_payload(char *payload, size_t size, const char *stage,
	const struct ltfs_commit_receipt *receipt)
{
	int length;
	unsigned int i;
	size_t used;

	length = snprintf(payload, size,
		"{\"schema\":1,\"stage\":\"%s\","
		"\"operation_id\":\"%s\",\"volume_uuid\":\"%s\","
		"\"prior_generation\":%llu,\"new_generation\":%llu,"
		"\"bytes_valid\":%s,\"bytes\":%llu,"
		"\"files_valid\":%s,\"files\":%llu,\"phase_duration_ns\":[",
		stage, receipt->operation_id, receipt->volume_uuid,
		(unsigned long long)receipt->prior_generation,
		(unsigned long long)receipt->new_generation,
		receipt->bytes_valid ? "true" : "false",
		(unsigned long long)receipt->bytes,
		receipt->files_valid ? "true" : "false",
		(unsigned long long)receipt->files);
	if (length < 0 || (size_t)length >= size)
		return -EOVERFLOW;
	used = (size_t)length;
	for (i = 0; i < LTFS_FINALIZATION_PHASE_COUNT; ++i) {
		length = snprintf(payload + used, size - used, "%s%llu",
			i ? "," : "",
			(unsigned long long)receipt->phase_duration_ns[i]);
		if (length < 0 || (size_t)length >= size - used)
			return -EOVERFLOW;
		used += (size_t)length;
	}
	length = snprintf(payload + used, size - used,
		"],\"capture_duration_ns\":%llu,"
		"\"device_close_duration_ns\":%llu,"
		"\"device_close_result_valid\":%s,\"device_close_result\":%d,"
		"\"catalog_ack_duration_ns\":%llu,\"media_committed\":%s,"
		"\"catalog_acknowledged\":%s,\"cleanup_failed\":%s,"
		"\"result\":%d}\n",
		(unsigned long long)receipt->capture_duration_ns,
		(unsigned long long)receipt->device_close_duration_ns,
		receipt->device_close_result_valid ? "true" : "false",
		receipt->device_close_result,
		(unsigned long long)receipt->catalog_ack_duration_ns,
		receipt->media_committed ? "true" : "false",
		receipt->catalog_acknowledged ? "true" : "false",
		receipt->cleanup_failed ? "true" : "false", receipt->result);
	if (length < 0 || (size_t)length >= size - used)
		return -EOVERFLOW;
	return (int)(used + (size_t)length);
}

static bool valid_identity_text(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	size_t length;

	if (!value || !value[0])
		return false;
	length = strlen(value);
	if (length >= LTFS_STANDALONE_IDENTITY_TEXT_MAX ||
		value[0] == ' ' || value[length - 1] == ' ')
		return false;
	for (; *cursor; ++cursor)
		if (*cursor < 0x20 || *cursor > 0x7e)
			return false;
	return true;
}

static int append_payload_text(char *payload, size_t size, size_t *used,
	const char *text)
{
	size_t length = strlen(text);
	if (length >= size - *used)
		return -EOVERFLOW;
	memcpy(payload + *used, text, length);
	*used += length;
	payload[*used] = '\0';
	return 0;
}

static int append_payload_json_string(char *payload, size_t size,
	size_t *used, const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	char escaped[7];

	if (append_payload_text(payload, size, used, "\"") < 0)
		return -EOVERFLOW;
	for (; *cursor; ++cursor) {
		if (*cursor == '"' || *cursor == '\\') {
			escaped[0] = '\\';
			escaped[1] = (char)*cursor;
			escaped[2] = '\0';
		} else if (*cursor < 0x20) {
			snprintf(escaped, sizeof(escaped), "\\u%04x", *cursor);
		} else {
			escaped[0] = (char)*cursor;
			escaped[1] = '\0';
		}
		if (append_payload_text(payload, size, used, escaped) < 0)
			return -EOVERFLOW;
	}
	return append_payload_text(payload, size, used, "\"");
}

static int ready_payload(char *payload, size_t size,
	const struct ltfs_standalone_ready_identity *identity)
{
	char prefix[256];
	size_t used = 0;
	int length;
	int ret;

	if (!identity || !valid_identifier(identity->operation_id) ||
		!valid_identifier(identity->volume_uuid) ||
		identity->prior_generation == 0 ||
		!valid_identity_text(identity->drive_serial) ||
		!identity->mam_barcode_valid ||
		!valid_identity_text(identity->mam_barcode) ||
		!identity->mam_volume_serial_valid ||
		!valid_identity_text(identity->mam_volume_serial) ||
		!identity->ltfs_volume_label_valid ||
		!valid_identity_text(identity->ltfs_volume_label))
		return -EINVAL;
	length = snprintf(prefix, sizeof(prefix),
		"{\"schema\":1,\"stage\":\"ready\","
		"\"operation_id\":\"%s\",\"volume_uuid\":\"%s\","
		"\"prior_generation\":%llu,\"read_only\":%s,"
		"\"drive_serial\":",
		identity->operation_id, identity->volume_uuid,
		(unsigned long long)identity->prior_generation,
		identity->read_only ? "true" : "false");
	if (length < 0 || (size_t)length >= sizeof(prefix))
		return -EOVERFLOW;
	ret = append_payload_text(payload, size, &used, prefix);
	if (!ret)
		ret = append_payload_json_string(payload, size, &used,
			identity->drive_serial);
	if (!ret)
		ret = append_payload_text(payload, size, &used,
			",\"mam_barcode\":");
	if (!ret)
		ret = append_payload_json_string(payload, size, &used,
			identity->mam_barcode);
	if (!ret)
		ret = append_payload_text(payload, size, &used,
			",\"mam_volume_serial\":");
	if (!ret)
		ret = append_payload_json_string(payload, size, &used,
			identity->mam_volume_serial);
	if (!ret)
		ret = append_payload_text(payload, size, &used,
			",\"ltfs_volume_label\":");
	if (!ret)
		ret = append_payload_json_string(payload, size, &used,
			identity->ltfs_volume_label);
	if (!ret)
		ret = append_payload_text(payload, size, &used, "}\n");
	if (ret < 0)
		return ret;
	return (int)used;
}

static int open_receipt_directory(const char *path, char **directory,
	const char **basename)
{
	char *slash;
	int dirfd;

	if (!path || path[0] != '/')
		return -EINVAL;
	*directory = strdup(path);
	if (!*directory)
		return -ENOMEM;
	slash = strrchr(*directory, '/');
	if (!slash || !slash[1]) {
		free(*directory);
		*directory = NULL;
		return -EINVAL;
	}
	*basename = path + (slash - *directory) + 1;
	if (!strcmp(*basename, ".") || !strcmp(*basename, "..")) {
		free(*directory);
		*directory = NULL;
		return -EINVAL;
	}
	if (slash == *directory)
		(*directory)[1] = '\0';
	else
		*slash = '\0';
	dirfd = open(*directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (dirfd < 0) {
		int ret = -errno;
		free(*directory);
		*directory = NULL;
		return ret;
	}
	return dirfd;
}

static int verify_receipt_directory(int dirfd, const char *directory)
{
	struct stat descriptor_status;
	struct stat path_status;

	if (dirfd < 0 || !directory || fstat(dirfd, &descriptor_status) < 0 ||
		lstat(directory, &path_status) < 0)
		return -errno;
	if (!S_ISDIR(descriptor_status.st_mode) ||
		(descriptor_status.st_mode & 0777) != 0700 ||
		descriptor_status.st_uid != geteuid() ||
		descriptor_status.st_gid != getegid() ||
		descriptor_status.st_nlink != 2 ||
		!S_ISDIR(path_status.st_mode) ||
		descriptor_status.st_dev != path_status.st_dev ||
		descriptor_status.st_ino != path_status.st_ino)
		return -EPERM;
	return 0;
}

static int write_payload_at(int dirfd, const char *name, const char *payload,
	size_t length)
{
	char temporary[PATH_MAX];
	int fd;
	int ret;

	if (snprintf(temporary, sizeof(temporary), "%s.tmp", name) >=
		(int)sizeof(temporary))
		return -ENAMETOOLONG;
	fd = openat(dirfd, temporary,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return -errno;
	if (fchmod(fd, 0600) != 0)
		ret = -errno;
	else {
		ret = write_all(fd, payload, length);
		if (ret == 0 && fsync(fd) != 0)
			ret = -errno;
	}
	if (close(fd) != 0 && ret == 0)
		ret = -errno;
	if (ret == 0 && linkat(dirfd, temporary, dirfd, name, 0) != 0)
		ret = -errno;
	if (ret == 0 && fsync(dirfd) != 0)
		ret = -errno;
	if (unlinkat(dirfd, temporary, 0) != 0 && ret == 0)
		ret = -errno;
	if (ret == 0 && fsync(dirfd) != 0)
		ret = -errno;
	return ret;
}

static int write_receipt_at(int dirfd, const char *name, const char *stage,
	const struct ltfs_commit_receipt *receipt)
{
	char payload[8192];
	int length = receipt_payload(payload, sizeof(payload), stage, receipt);
	return length < 0 ? length :
		write_payload_at(dirfd, name, payload, (size_t)length);
}

static bool receipt_counters_valid(const struct ltfs_commit_receipt *receipt)
{
	return receipt->new_generation != 0 &&
		receipt->new_generation >= receipt->prior_generation &&
		(receipt->bytes_valid || receipt->bytes == 0) &&
		(receipt->files_valid || receipt->files == 0) &&
		(receipt->cleanup_failed == (receipt->result != 0));
}

static int verify_receipt_at(int dirfd, const char *name, const char *stage,
	const struct ltfs_commit_receipt *receipt)
{
	char actual[8192];
	char expected[8192];
	struct stat status;
	size_t used = 0;
	int expected_length = receipt_payload(expected, sizeof(expected), stage,
		receipt);
	int fd;

	if (expected_length < 0)
		return expected_length;
	fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
		(status.st_mode & 0777) != 0600 || status.st_uid != geteuid() ||
		status.st_gid != getegid() ||
		status.st_nlink != 1) {
		close(fd);
		return -EPERM;
	}
	while (used < sizeof(actual)) {
		ssize_t length = read(fd, actual + used, sizeof(actual) - used);
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
	if (close(fd) != 0)
		return -errno;
	if (used == sizeof(actual) || used != (size_t)expected_length ||
		memcmp(actual, expected, used))
		return -EPROTO;
	return 0;
}

static int verify_ready_at(int dirfd, const char *name,
	const struct ltfs_standalone_ready_identity *identity)
{
	char actual[4096];
	char expected[4096];
	struct stat status;
	size_t used = 0;
	int expected_length = ready_payload(expected, sizeof(expected), identity);
	int fd;

	if (expected_length < 0)
		return expected_length;
	fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
		(status.st_mode & 0777) != 0600 || status.st_uid != geteuid() ||
		status.st_gid != getegid() ||
		status.st_nlink != 1) {
		close(fd);
		return -EPERM;
	}
	while (used < sizeof(actual)) {
		ssize_t length = read(fd, actual + used, sizeof(actual) - used);
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
	if (close(fd) != 0)
		return -errno;
	if (used == sizeof(actual) || used != (size_t)expected_length ||
		memcmp(actual, expected, used))
		return -EPROTO;
	return 0;
}

int ltfs_standalone_receipt_ready(const char *path,
	const struct ltfs_standalone_ready_identity *identity)
{
	char ready[PATH_MAX];
	char payload[4096];
	char *directory = NULL;
	const char *basename = NULL;
	int dirfd;
	int length;
	int ret;

	if (!path || path[0] != '/')
		return -EINVAL;
	length = ready_payload(payload, sizeof(payload), identity);
	if (length < 0)
		return length;
	dirfd = open_receipt_directory(path, &directory, &basename);
	if (dirfd < 0)
		return dirfd;
	ret = verify_receipt_directory(dirfd, directory);
	if (ret < 0)
		goto out;
	if (snprintf(ready, sizeof(ready), "%s.ready", basename) >=
		(int)sizeof(ready)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	ret = write_payload_at(dirfd, ready, payload, (size_t)length);
	if (ret == 0)
		ret = verify_receipt_directory(dirfd, directory);
out:
	close(dirfd);
	free(directory);
	return ret;
}

int ltfs_standalone_receipt_persist(const char *path,
	const struct ltfs_standalone_ready_identity *identity,
	const struct ltfs_commit_receipt *receipt,
	struct ltfs_commit_ack *ack)
{
	char pending[PATH_MAX];
	char ready[PATH_MAX];
	char *directory = NULL;
	const char *basename = NULL;
	struct stat status;
	int dirfd;
	int length;
	int ret;

	if (!path || path[0] != '/' || !identity || !receipt || !ack ||
		!valid_identifier(receipt->operation_id) ||
		!valid_identifier(receipt->volume_uuid) || !receipt->media_committed ||
		!receipt_counters_valid(receipt) ||
		(identity->read_only &&
		 receipt->new_generation != receipt->prior_generation) ||
		strcmp(identity->operation_id, receipt->operation_id) ||
		strcmp(identity->volume_uuid, receipt->volume_uuid) ||
		identity->prior_generation != receipt->prior_generation ||
		receipt->catalog_acknowledged ||
		receipt->device_close_result_valid || receipt->device_close_result != 0)
		return -EINVAL;
	memset(ack, 0, sizeof(*ack));
	dirfd = open_receipt_directory(path, &directory, &basename);
	if (dirfd < 0)
		return dirfd;
	ret = verify_receipt_directory(dirfd, directory);
	if (ret < 0)
		goto out;
	length = snprintf(pending, sizeof(pending), "%s.pending", basename);
	if (length < 0 || length >= (int)sizeof(pending)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	length = snprintf(ready, sizeof(ready), "%s.ready", basename);
	if (length < 0 || length >= (int)sizeof(ready)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	if (fstatat(dirfd, basename, &status, AT_SYMLINK_NOFOLLOW) == 0) {
		ret = -EEXIST;
		goto out;
	}
	if (errno != ENOENT) {
		ret = -errno;
		goto out;
	}
	ret = verify_ready_at(dirfd, ready, identity);
	if (ret < 0)
		goto out;
	ret = write_receipt_at(dirfd, pending, "prepared", receipt);
	if (ret < 0)
		goto out;
	if (fsync(dirfd) != 0) {
		ret = -errno;
		(void)unlinkat(dirfd, pending, 0);
		goto out;
	}
	strncpy(ack->operation_id, receipt->operation_id, sizeof(ack->operation_id) - 1);
	strncpy(ack->volume_uuid, receipt->volume_uuid, sizeof(ack->volume_uuid) - 1);
	ack->generation = receipt->new_generation;
	ack->durable = true;
	ret = verify_receipt_directory(dirfd, directory);
out:
	close(dirfd);
	free(directory);
	return ret;
}

int ltfs_standalone_receipt_finalize(const char *path,
	const struct ltfs_standalone_ready_identity *identity,
	const struct ltfs_commit_receipt *prepared_receipt,
	const struct ltfs_commit_receipt *terminal_receipt)
{
	char pending[PATH_MAX];
	char ready[PATH_MAX];
	char *directory = NULL;
	const char *basename = NULL;
	struct stat status;
	int dirfd;
	int length;
	int ret;

	if (!identity || !prepared_receipt || !terminal_receipt ||
		!valid_identifier(terminal_receipt->operation_id) ||
		!valid_identifier(terminal_receipt->volume_uuid) ||
		!terminal_receipt->media_committed ||
		!receipt_counters_valid(terminal_receipt) ||
		!terminal_receipt->catalog_acknowledged ||
		!terminal_receipt->device_close_result_valid ||
		strcmp(identity->operation_id, terminal_receipt->operation_id) ||
		strcmp(identity->volume_uuid, terminal_receipt->volume_uuid) ||
		identity->prior_generation != terminal_receipt->prior_generation ||
		strcmp(prepared_receipt->operation_id,
			terminal_receipt->operation_id) ||
		strcmp(prepared_receipt->volume_uuid,
			terminal_receipt->volume_uuid) ||
		prepared_receipt->prior_generation !=
			terminal_receipt->prior_generation ||
		prepared_receipt->new_generation !=
			terminal_receipt->new_generation)
		return -EINVAL;
	dirfd = open_receipt_directory(path, &directory, &basename);
	if (dirfd < 0)
		return dirfd;
	ret = verify_receipt_directory(dirfd, directory);
	if (ret < 0)
		goto out;
	length = snprintf(pending, sizeof(pending), "%s.pending", basename);
	if (length < 0 || length >= (int)sizeof(pending)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	length = snprintf(ready, sizeof(ready), "%s.ready", basename);
	if (length < 0 || length >= (int)sizeof(ready)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	if (fstatat(dirfd, basename, &status, AT_SYMLINK_NOFOLLOW) == 0) {
		ret = -EEXIST;
		goto out;
	}
	if (errno != ENOENT) {
		ret = -errno;
		goto out;
	}
	if (fstatat(dirfd, pending, &status, AT_SYMLINK_NOFOLLOW) < 0) {
		ret = -errno;
		goto out;
	}
	ret = verify_ready_at(dirfd, ready, identity);
	if (ret < 0)
		goto out;
	ret = verify_receipt_at(dirfd, pending, "prepared", prepared_receipt);
	if (ret < 0)
		goto out;
	ret = write_receipt_at(dirfd, basename, "terminal", terminal_receipt);
	if (ret < 0)
		goto out;
	if (fsync(dirfd) != 0) {
		ret = -errno;
		goto out;
	}
	if (unlinkat(dirfd, pending, 0) != 0 ||
		unlinkat(dirfd, ready, 0) != 0 || fsync(dirfd) != 0) {
		ret = -errno;
		goto out;
	}
	ret = verify_receipt_directory(dirfd, directory);
out:
	close(dirfd);
	free(directory);
	return ret;
}
