/* SPDX-License-Identifier: BSD-3-Clause */

#include "operation_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOCK_RECORD_MAX 1024

struct lock_record {
	pid_t pid;
	char boot_id[LTFS_DEVICE_BOOT_ID_MAX];
	uint64_t start_ticks;
	char operation_id[LTFS_DEVICE_IDENTITY_TEXT_MAX];
	enum ltfs_command_class command_class;
	char target_sha256[LTFS_DEVICE_SHA256_HEX_SIZE];
	uint64_t created_unix_ns;
};

static pthread_mutex_t temporary_name_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long temporary_name_counter;

static bool valid_uuid(const char *value)
{
	size_t i;
	if (!value || strlen(value) != 36)
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

static bool valid_sha256(const char *value)
{
	size_t i;
	if (!value || strlen(value) != 64)
		return false;
	for (i = 0; i < 64; ++i)
		if (!((value[i] >= '0' && value[i] <= '9') ||
			(value[i] >= 'a' && value[i] <= 'f')))
			return false;
	return true;
}

static const char *command_class_name(enum ltfs_command_class command_class)
{
	switch (command_class) {
	case LTFS_COMMAND_CLASS_MOUNT:
		return "mount";
	case LTFS_COMMAND_CLASS_CHECK:
		return "check";
	case LTFS_COMMAND_CLASS_DIAGNOSTIC:
		return "diagnostic";
	case LTFS_COMMAND_CLASS_UNLOAD:
		return "unload";
	case LTFS_COMMAND_CLASS_FIRMWARE:
		return "firmware";
	case LTFS_COMMAND_CLASS_READ_ONLY_INFO:
		return "read-only-info";
	default:
		return NULL;
	}
}

static enum ltfs_command_class parse_command_class(const char *name)
{
	enum ltfs_command_class value;
	for (value = LTFS_COMMAND_CLASS_MOUNT;
		value <= LTFS_COMMAND_CLASS_READ_ONLY_INFO; ++value)
		if (!strcmp(name, command_class_name(value)))
			return value;
	return 0;
}

static int write_all(int fd, const char *buffer, size_t length)
{
	size_t offset = 0;
	while (offset < length) {
		ssize_t written = write(fd, buffer + offset, length - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return written < 0 ? -errno : -EIO;
		offset += (size_t)written;
	}
	return 0;
}

static int read_record_fd(int fd, char *buffer, size_t size,
	struct stat *status)
{
	ssize_t length;
	if (fstat(fd, status) < 0)
		return -errno;
	if (!S_ISREG(status->st_mode) || (status->st_mode & 0777) != 0600)
		return LTFS_OPERATION_LOCK_CORRUPT;
	do {
		length = pread(fd, buffer, size - 1, 0);
	} while (length < 0 && errno == EINTR);
	if (length <= 0 || length == (ssize_t)size - 1)
		return length < 0 ? -errno : LTFS_OPERATION_LOCK_CORRUPT;
	buffer[length] = '\0';
	return (int)length;
}

static int parse_record(const char *buffer, size_t length,
	struct lock_record *record)
{
	char command[32];
	long pid;
	unsigned long long start_ticks;
	unsigned long long created;
	int consumed = 0;
	int matched = sscanf(buffer,
		"{\"pid\":%ld,\"boot_id\":\"%36[^\"]\","
		"\"start_ticks\":%llu,\"operation_id\":\"%255[^\"]\","
		"\"command_class\":\"%31[^\"]\",\"target_sha256\":\"%64[^\"]\","
		"\"created_unix_ns\":%llu}\n%n",
		&pid, record->boot_id, &start_ticks, record->operation_id,
		command, record->target_sha256, &created, &consumed);
	if (matched != 7 || consumed != (int)length || pid <= 0 ||
		!valid_uuid(record->boot_id) || !valid_uuid(record->operation_id) ||
		!valid_sha256(record->target_sha256))
		return LTFS_OPERATION_LOCK_CORRUPT;
	record->command_class = parse_command_class(command);
	if (!record->command_class)
		return LTFS_OPERATION_LOCK_CORRUPT;
	record->pid = (pid_t)pid;
	record->start_ticks = (uint64_t)start_ticks;
	record->created_unix_ns = (uint64_t)created;
	return 0;
}

static int format_record(const struct lock_record *record, char *buffer,
	size_t size)
{
	const char *command = command_class_name(record->command_class);
	int length;
	if (!command)
		return -EINVAL;
	length = snprintf(buffer, size,
		"{\"pid\":%ld,\"boot_id\":\"%s\",\"start_ticks\":%llu,"
		"\"operation_id\":\"%s\",\"command_class\":\"%s\","
		"\"target_sha256\":\"%s\",\"created_unix_ns\":%llu}\n",
		(long)record->pid, record->boot_id,
		(unsigned long long)record->start_ticks, record->operation_id,
		command, record->target_sha256,
		(unsigned long long)record->created_unix_ns);
	return length >= 0 && length < (int)size ? length : -ENAMETOOLONG;
}

static int read_start_ticks(const char *proc_root, pid_t pid,
	uint64_t *start_ticks)
{
	char path[PATH_MAX];
	char buffer[4096];
	char *right_parenthesis;
	char *save = NULL;
	char *token;
	unsigned int field = 3;
	int fd;
	ssize_t length;
	int path_length = snprintf(path, sizeof(path), "%s/%ld/stat",
		proc_root, (long)pid);
	if (path_length < 0 || path_length >= (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -errno;
	do {
		length = read(fd, buffer, sizeof(buffer) - 1);
	} while (length < 0 && errno == EINTR);
	if (close(fd) < 0 && length >= 0)
		return -errno;
	if (length <= 0 || length == (ssize_t)sizeof(buffer) - 1)
		return length < 0 ? -errno : -EINVAL;
	buffer[length] = '\0';
	right_parenthesis = strrchr(buffer, ')');
	if (!right_parenthesis || right_parenthesis[1] != ' ')
		return -EINVAL;
	token = strtok_r(right_parenthesis + 2, " ", &save);
	while (token) {
		if (field == 22) {
			char *end;
			unsigned long long value;
			errno = 0;
			value = strtoull(token, &end, 10);
			if (errno || end == token || (*end && *end != '\n'))
				return -EINVAL;
			*start_ticks = (uint64_t)value;
			return 0;
		}
		++field;
		token = strtok_r(NULL, " ", &save);
	}
	return -EINVAL;
}

static int fill_own_record(const struct ltfs_device_identity *identity,
	const char *operation_id, enum ltfs_command_class command_class,
	struct lock_record *record)
{
	struct timespec now;
	int ret;
	if (!valid_uuid(operation_id) || !valid_uuid(identity->boot_id) ||
		!valid_sha256(identity->target_sha256) ||
		!command_class_name(command_class))
		return -EINVAL;
	memset(record, 0, sizeof(*record));
	record->pid = getpid();
	ret = read_start_ticks(identity->proc_root, record->pid,
		&record->start_ticks);
	if (ret < 0)
		return ret;
	if (clock_gettime(CLOCK_REALTIME, &now) < 0)
		return -errno;
	record->created_unix_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
	strcpy(record->boot_id, identity->boot_id);
	strcpy(record->operation_id, operation_id);
	strcpy(record->target_sha256, identity->target_sha256);
	record->command_class = command_class;
	return 0;
}

static int record_state(const struct lock_record *record,
	const struct ltfs_device_identity *identity)
{
	uint64_t current_start;
	int ret;
	if (strcmp(record->target_sha256, identity->target_sha256))
		return LTFS_OPERATION_LOCK_CORRUPT;
	if (strcmp(record->boot_id, identity->boot_id))
		return 1;
	ret = read_start_ticks(identity->proc_root, record->pid, &current_start);
	if (ret == -ENOENT)
		return 1;
	if (ret < 0)
		return LTFS_OPERATION_LOCK_BUSY;
	return current_start == record->start_ticks ?
		LTFS_OPERATION_LOCK_BUSY : 1;
}

static int create_record_at(int directory_fd, const char *name,
	const char *contents, size_t length, struct stat *status)
{
	int fd = openat(directory_fd, name,
		O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	int ret;
	if (fd < 0)
		return -errno;
	ret = write_all(fd, contents, length);
	if (ret == 0 && fsync(fd) < 0)
		ret = -errno;
	if (ret == 0 && fstat(fd, status) < 0)
		ret = -errno;
	if (close(fd) < 0 && ret == 0)
		ret = -errno;
	if (ret < 0)
		unlinkat(directory_fd, name, 0);
	return ret;
}

static int read_existing_at(int directory_fd, const char *name,
	char *contents, struct stat *status, struct lock_record *record)
{
	int fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	int length;
	int ret;
	if (fd < 0)
		return errno == ENOENT ? -ENOENT : LTFS_OPERATION_LOCK_CORRUPT;
	length = read_record_fd(fd, contents, LOCK_RECORD_MAX, status);
	if (close(fd) < 0 && length >= 0)
		return -errno;
	if (length < 0)
		return length;
	ret = parse_record(contents, (size_t)length, record);
	return ret < 0 ? ret : length;
}

static int remember_lock(const struct ltfs_device_identity *identity,
	const struct lock_record *record, const char *path, const struct stat *status,
	struct ltfs_operation_lock *lock)
{
	if (strlen(path) >= sizeof(lock->path) ||
		strlen(identity->proc_root) >= sizeof(lock->proc_root))
		return -ENAMETOOLONG;
	memset(lock, 0, sizeof(*lock));
	lock->owned = true;
	strcpy(lock->path, path);
	strcpy(lock->proc_root, identity->proc_root);
	strcpy(lock->boot_id, record->boot_id);
	strcpy(lock->operation_id, record->operation_id);
	strcpy(lock->target_sha256, record->target_sha256);
	lock->command_class = record->command_class;
	lock->pid = record->pid;
	lock->start_ticks = record->start_ticks;
	lock->created_unix_ns = record->created_unix_ns;
	lock->record_device = status->st_dev;
	lock->record_inode = status->st_ino;
	return 0;
}

static int replace_stale_record(int directory_fd, const char *name,
	const char *path, const struct ltfs_device_identity *identity,
	const struct lock_record *own_record, const char *own_contents,
	size_t own_length, struct ltfs_operation_lock *lock)
{
	char current_contents[LOCK_RECORD_MAX];
	char verify_contents[LOCK_RECORD_MAX];
	char temporary[128];
	struct lock_record current_record;
	struct lock_record verify_record;
	struct stat current_status;
	struct stat verify_status;
	struct stat own_status;
	unsigned long counter;
	int length;
	int state;
	int ret;

	length = read_existing_at(directory_fd, name, current_contents,
		&current_status, &current_record);
	if (length < 0) {
		ret = length == -ENOENT ? -EAGAIN : length;
		goto out;
	}
	state = record_state(&current_record, identity);
	if (state != 1) {
		ret = state;
		goto out;
	}
	pthread_mutex_lock(&temporary_name_mutex);
	counter = ++temporary_name_counter;
	pthread_mutex_unlock(&temporary_name_mutex);
	ret = snprintf(temporary, sizeof(temporary), ".%s.%ld.%lu.tmp",
		identity->target_sha256, (long)getpid(), counter);
	if (ret < 0 || ret >= (int)sizeof(temporary)) {
		ret = -ENAMETOOLONG;
		goto out;
	}
	ret = create_record_at(directory_fd, temporary, own_contents,
		own_length, &own_status);
	if (ret < 0)
		goto out;
	length = read_existing_at(directory_fd, name, verify_contents,
		&verify_status, &verify_record);
	if (length < 0 || verify_status.st_dev != current_status.st_dev ||
		verify_status.st_ino != current_status.st_ino ||
		strcmp(verify_contents, current_contents)) {
		unlinkat(directory_fd, temporary, 0);
		ret = LTFS_OPERATION_LOCK_BUSY;
		goto out;
	}
	if (renameat(directory_fd, temporary, directory_fd, name) < 0) {
		ret = -errno;
		unlinkat(directory_fd, temporary, 0);
		goto out;
	}
	if (fsync(directory_fd) < 0) {
		ret = -errno;
		goto out;
	}
	ret = remember_lock(identity, own_record, path, &own_status, lock);
out:
	return ret;
}

int ltfs_operation_lock_acquire(const struct ltfs_device_identity *identity,
	const char *operation_id, enum ltfs_command_class command_class,
	struct ltfs_operation_lock *lock)
{
	char name[80];
	char path[PATH_MAX];
	char own_contents[LOCK_RECORD_MAX];
	char existing_contents[LOCK_RECORD_MAX];
	struct lock_record own_record;
	struct lock_record existing_record;
	struct stat own_status;
	struct stat existing_status;
	int directory_fd;
	int own_length;
	int existing_length;
	int ret;

	if (!identity || !operation_id || !lock || !identity->lock_root[0] ||
		!identity->proc_root[0])
		return -EINVAL;
	memset(lock, 0, sizeof(*lock));
	ret = fill_own_record(identity, operation_id, command_class, &own_record);
	if (ret < 0)
		return ret;
	own_length = format_record(&own_record, own_contents, sizeof(own_contents));
	if (own_length < 0)
		return own_length;
	ret = snprintf(name, sizeof(name), "%s.json", identity->target_sha256);
	if (ret < 0 || ret >= (int)sizeof(name))
		return -ENAMETOOLONG;
	ret = snprintf(path, sizeof(path), "%s/%s", identity->lock_root, name);
	if (ret < 0 || ret >= (int)sizeof(path))
		return -ENAMETOOLONG;
	directory_fd = open(identity->lock_root,
		O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (directory_fd < 0)
		return -errno;
	if (flock(directory_fd, LOCK_EX) < 0) {
		ret = -errno;
		close(directory_fd);
		return ret;
	}
	ret = create_record_at(directory_fd, name, own_contents,
		(size_t)own_length, &own_status);
	if (ret == 0) {
		if (fsync(directory_fd) < 0)
			ret = -errno;
		else
			ret = remember_lock(identity, &own_record, path, &own_status, lock);
		flock(directory_fd, LOCK_UN);
		close(directory_fd);
		return ret;
	}
	if (ret != -EEXIST) {
		flock(directory_fd, LOCK_UN);
		close(directory_fd);
		return ret;
	}
	existing_length = read_existing_at(directory_fd, name, existing_contents,
		&existing_status, &existing_record);
	if (existing_length < 0) {
		flock(directory_fd, LOCK_UN);
		close(directory_fd);
		return existing_length;
	}
	ret = record_state(&existing_record, identity);
	if (ret == 1)
		ret = replace_stale_record(directory_fd, name, path, identity,
			&own_record, own_contents, (size_t)own_length, lock);
	flock(directory_fd, LOCK_UN);
	close(directory_fd);
	return ret;
}

void ltfs_operation_lock_release(struct ltfs_operation_lock *lock)
{
	char path[PATH_MAX];
	char contents[LOCK_RECORD_MAX];
	char *slash;
	const char *name;
	struct lock_record record;
	struct stat status;
	int directory_fd;
	int length;
	if (!lock || !lock->owned || strlen(lock->path) >= sizeof(path))
		return;
	strcpy(path, lock->path);
	slash = strrchr(path, '/');
	if (!slash || slash == path)
		return;
	*slash = '\0';
	name = slash + 1;
	directory_fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
	if (directory_fd < 0)
		return;
	if (flock(directory_fd, LOCK_EX) < 0) {
		close(directory_fd);
		return;
	}
	length = read_existing_at(directory_fd, name, contents, &status, &record);
	if (length >= 0 && status.st_dev == lock->record_device &&
		status.st_ino == lock->record_inode && record.pid == lock->pid &&
		record.start_ticks == lock->start_ticks &&
		record.command_class == lock->command_class &&
		record.created_unix_ns == lock->created_unix_ns &&
		!strcmp(record.boot_id, lock->boot_id) &&
		!strcmp(record.operation_id, lock->operation_id) &&
		!strcmp(record.target_sha256, lock->target_sha256) &&
		unlinkat(directory_fd, name, 0) == 0) {
		fsync(directory_fd);
		lock->owned = false;
	}
	flock(directory_fd, LOCK_UN);
	close(directory_fd);
}
