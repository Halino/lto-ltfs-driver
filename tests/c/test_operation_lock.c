/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "device_identity.h"
#include "fuse_foreground.h"
#include "operation_lock.h"

#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CONCURRENT_ACQUIRERS 100

struct lock_fixture {
	char root[PATH_MAX];
	char proc_root[PATH_MAX];
	char lock_root[PATH_MAX];
	char stat_path[PATH_MAX];
	struct ltfs_device_identity identity;
};

struct acquire_context {
	const struct ltfs_device_identity *identity;
	pthread_barrier_t *barrier;
	struct ltfs_operation_lock lock;
	int result;
};

int fuse_opt_insert_arg(struct fuse_args *args, int position,
	const char *argument)
{
	char **updated;
	int i;
	if (!args || !argument || position < 0 || position > args->argc)
		return -EINVAL;
	updated = calloc((size_t)args->argc + 2, sizeof(*updated));
	if (!updated)
		return -ENOMEM;
	for (i = 0; i < position; ++i)
		updated[i] = args->argv[i];
	updated[position] = strdup(argument);
	if (!updated[position]) {
		free(updated);
		return -ENOMEM;
	}
	for (i = position; i < args->argc; ++i)
		updated[i + 1] = args->argv[i];
	args->argv = updated;
	++args->argc;
	args->allocated = 1;
	return 0;
}

static int join_path(char *output, size_t size, const char *left,
	const char *right)
{
	char saved_left[PATH_MAX];
	int length;
	if (strlen(left) >= sizeof(saved_left))
		return -1;
	strcpy(saved_left, left);
	length = snprintf(output, size, "%s/%s", saved_left, right);
	return length >= 0 && length < (int)size ? 0 : -1;
}

static int make_directory(const char *path)
{
	if (mkdir(path, 0700) == 0 || errno == EEXIST)
		return 0;
	return -1;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	size_t length = strlen(text);
	ssize_t written;
	if (fd < 0)
		return -1;
	written = write(fd, text, length);
	if (close(fd) < 0)
		return -1;
	return written == (ssize_t)length ? 0 : -1;
}

static int write_process_stat(struct lock_fixture *fixture,
	unsigned long long start_ticks)
{
	char contents[1024];
	int length = snprintf(contents, sizeof(contents),
		"%ld (ltfs test worker) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 %llu 0\n",
		(long)getpid(), start_ticks);
	return length > 0 && length < (int)sizeof(contents) ?
		write_text(fixture->stat_path, contents) : -1;
}

static int fixture_init(struct lock_fixture *fixture)
{
	char template[] = "/tmp/lto-ltfs-lock.XXXXXX";
	char path[PATH_MAX];
	char pid_text[32];
	char *root = mkdtemp(template);
	if (!root)
		return -1;
	memset(fixture, 0, sizeof(*fixture));
	strncpy(fixture->root, root, sizeof(fixture->root) - 1);
	if (join_path(fixture->proc_root, sizeof(fixture->proc_root), root, "proc") < 0 ||
		join_path(fixture->lock_root, sizeof(fixture->lock_root), root, "locks") < 0 ||
		make_directory(fixture->proc_root) < 0 ||
		make_directory(fixture->lock_root) < 0)
		return -1;
	snprintf(pid_text, sizeof(pid_text), "%ld", (long)getpid());
	if (join_path(path, sizeof(path), fixture->proc_root, pid_text) < 0 ||
		make_directory(path) < 0 ||
		join_path(fixture->stat_path, sizeof(fixture->stat_path), path, "stat") < 0 ||
		write_process_stat(fixture, 4242) < 0)
		return -1;

	strncpy(fixture->identity.proc_root, fixture->proc_root,
		sizeof(fixture->identity.proc_root) - 1);
	strncpy(fixture->identity.lock_root, fixture->lock_root,
		sizeof(fixture->identity.lock_root) - 1);
	strcpy(fixture->identity.boot_id, "11111111-2222-4333-8444-555555555555");
	strcpy(fixture->identity.target_sha256,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	return 0;
}

static int lock_path(const struct lock_fixture *fixture, char *path, size_t size)
{
	char name[80];
	snprintf(name, sizeof(name), "%s.json", fixture->identity.target_sha256);
	return join_path(path, size, fixture->lock_root, name);
}

static int test_first_acquisition_live_conflict_and_owned_release(void)
{
	struct lock_fixture fixture;
	struct ltfs_operation_lock owner;
	struct ltfs_operation_lock contender;
	char path[PATH_MAX];
	struct stat status;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000005",
		LTFS_COMMAND_CLASS_MOUNT, &owner), 0);
	CHECK_INT_EQ(lock_path(&fixture, path, sizeof(path)), 0);
	CHECK_INT_EQ(stat(path, &status), 0);
	CHECK_INT_EQ(status.st_mode & 0777, 0600);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000006",
		LTFS_COMMAND_CLASS_CHECK, &contender), LTFS_OPERATION_LOCK_BUSY);
	ltfs_operation_lock_release(&owner);
	CHECK_INT_EQ(access(path, F_OK), -1);
	CHECK_INT_EQ(errno, ENOENT);
	return 0;
}

static int test_read_only_info_class_is_serialized(void)
{
	struct lock_fixture fixture;
	struct ltfs_operation_lock owner;
	struct ltfs_operation_lock contender;
	char path[PATH_MAX];
	char record[1024] = {0};
	int fd;
	ssize_t length;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000105",
		LTFS_COMMAND_CLASS_READ_ONLY_INFO, &owner), 0);
	CHECK_INT_EQ(lock_path(&fixture, path, sizeof(path)), 0);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK_TRUE(fd >= 0);
	length = read(fd, record, sizeof(record) - 1);
	CHECK_TRUE(length > 0);
	CHECK_INT_EQ(close(fd), 0);
	CHECK_TRUE(strstr(record,
		"\"command_class\":\"read-only-info\"") != NULL);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000106",
		LTFS_COMMAND_CLASS_MOUNT, &contender), LTFS_OPERATION_LOCK_BUSY);
	ltfs_operation_lock_release(&owner);
	return 0;
}

static int test_pid_reuse_and_previous_boot_are_proven_stale(void)
{
	struct lock_fixture fixture;
	struct ltfs_operation_lock old_owner;
	struct ltfs_operation_lock new_owner;
	struct ltfs_device_identity next_boot;
	char path[PATH_MAX];
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000007",
		LTFS_COMMAND_CLASS_MOUNT, &old_owner), 0);
	CHECK_INT_EQ(write_process_stat(&fixture, 5252), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000008",
		LTFS_COMMAND_CLASS_CHECK, &new_owner), 0);
	CHECK_INT_EQ(lock_path(&fixture, path, sizeof(path)), 0);
	ltfs_operation_lock_release(&old_owner);
	CHECK_INT_EQ(access(path, F_OK), 0);
	ltfs_operation_lock_release(&new_owner);

	CHECK_INT_EQ(write_process_stat(&fixture, 6262), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000009",
		LTFS_COMMAND_CLASS_MOUNT, &old_owner), 0);
	next_boot = fixture.identity;
	strcpy(next_boot.boot_id, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee");
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&next_boot,
		"00000000-0000-4000-8000-000000000010",
		LTFS_COMMAND_CLASS_CHECK, &new_owner), 0);
	ltfs_operation_lock_release(&old_owner);
	CHECK_INT_EQ(access(path, F_OK), 0);
	ltfs_operation_lock_release(&new_owner);
	CHECK_INT_EQ(access(path, F_OK), -1);
	return 0;
}

static int test_corrupt_and_wrong_target_records_fail_closed(void)
{
	struct lock_fixture fixture;
	struct ltfs_operation_lock lock;
	char path[PATH_MAX];
	const char *wrong_target =
		"{\"pid\":1,\"boot_id\":\"11111111-2222-4333-8444-555555555555\","
		"\"start_ticks\":1,\"operation_id\":\"00000000-0000-4000-8000-000000000011\","
		"\"command_class\":\"mount\",\"target_sha256\":"
		"\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
		"\"created_unix_ns\":1}\n";
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(lock_path(&fixture, path, sizeof(path)), 0);
	CHECK_INT_EQ(write_text(path, "not-json\n"), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000012",
		LTFS_COMMAND_CLASS_MOUNT, &lock), LTFS_OPERATION_LOCK_CORRUPT);
	CHECK_INT_EQ(access(path, F_OK), 0);
	CHECK_INT_EQ(write_text(path, wrong_target), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000013",
		LTFS_COMMAND_CLASS_MOUNT, &lock), LTFS_OPERATION_LOCK_CORRUPT);
	CHECK_INT_EQ(access(path, F_OK), 0);
	return 0;
}

static int test_release_refuses_changed_ownership(void)
{
	struct lock_fixture fixture;
	struct ltfs_operation_lock owner;
	struct ltfs_operation_lock impostor;
	char path[PATH_MAX];
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000014",
		LTFS_COMMAND_CLASS_MOUNT, &owner), 0);
	impostor = owner;
	strcpy(impostor.operation_id, "00000000-0000-4000-8000-000000000099");
	ltfs_operation_lock_release(&impostor);
	CHECK_INT_EQ(lock_path(&fixture, path, sizeof(path)), 0);
	CHECK_INT_EQ(access(path, F_OK), 0);
	ltfs_operation_lock_release(&owner);
	CHECK_INT_EQ(access(path, F_OK), -1);
	return 0;
}

static void *concurrent_acquire(void *opaque)
{
	struct acquire_context *context = opaque;
	pthread_barrier_wait(context->barrier);
	context->result = ltfs_operation_lock_acquire(context->identity,
		"00000000-0000-4000-8000-000000000015",
		LTFS_COMMAND_CLASS_MOUNT, &context->lock);
	return NULL;
}

static bool foreground_present(const struct fuse_args *args)
{
	int i;
	for (i = 0; i < args->argc; ++i)
		if (!strcmp(args->argv[i], "-f"))
			return true;
	return false;
}

static int send_byte(int fd, char value)
{
	ssize_t result;
	do {
		result = write(fd, &value, 1);
	} while (result < 0 && errno == EINTR);
	return result == 1 ? 0 : -1;
}

static int receive_byte(int fd, char *value)
{
	ssize_t result;
	do {
		result = read(fd, value, 1);
	} while (result < 0 && errno == EINTR);
	return result == 1 ? 0 : -1;
}

static int test_forced_foreground_keeps_recorded_owner_live(void)
{
	struct lock_fixture fixture;
	struct ltfs_operation_lock contender;
	int ready[2];
	int daemon_continue[2];
	int release[2];
	pid_t owner;
	pid_t wait_result;
	int contender_result;
	int status;
	char ready_byte = 0;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	strcpy(fixture.identity.proc_root, "/proc");
	CHECK_INT_EQ(pipe(ready), 0);
	CHECK_INT_EQ(pipe(daemon_continue), 0);
	CHECK_INT_EQ(pipe(release), 0);
	owner = fork();
	CHECK_TRUE(owner >= 0);
	if (owner == 0) {
		struct ltfs_operation_lock held;
		char *arguments[] = { (char *)"ltfs", (char *)"/mnt/fake", NULL };
		struct fuse_args args = FUSE_ARGS_INIT(2, arguments);
		pid_t daemon;
		close(ready[0]);
		close(daemon_continue[1]);
		close(release[1]);
		if (ltfs_operation_lock_acquire(&fixture.identity,
			"00000000-0000-4000-8000-000000000016",
			LTFS_COMMAND_CLASS_MOUNT, &held) != 0)
			_exit(20);
		if (ltfs_force_foreground(&args) != 0)
			_exit(21);
		/* Model the only relevant fuse_main behavior: fork unless -f. */
		if (!foreground_present(&args)) {
			daemon = fork();
			if (daemon < 0)
				_exit(22);
			if (daemon > 0) {
				if (send_byte(ready[1], 'D') < 0)
					_exit(23);
				_exit(0);
			}
			if (receive_byte(daemon_continue[0], &ready_byte) < 0)
				_exit(24);
		}
		close(daemon_continue[0]);
		if (send_byte(ready[1], 'R') < 0 ||
			receive_byte(release[0], &ready_byte) < 0)
			_exit(25);
		ltfs_operation_lock_release(&held);
		_exit(0);
	}
	close(ready[1]);
	close(daemon_continue[0]);
	close(release[0]);
	CHECK_INT_EQ(receive_byte(ready[0], &ready_byte), 0);
	if (ready_byte == 'D') {
		CHECK_INT_EQ(waitpid(owner, &status, 0), owner);
		CHECK_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
		wait_result = owner;
		CHECK_INT_EQ(send_byte(daemon_continue[1], 'C'), 0);
		CHECK_INT_EQ(receive_byte(ready[0], &ready_byte), 0);
	} else
		wait_result = waitpid(owner, &status, WNOHANG);
	contender_result = ltfs_operation_lock_acquire(&fixture.identity,
		"00000000-0000-4000-8000-000000000017",
		LTFS_COMMAND_CLASS_CHECK, &contender);
	CHECK_INT_EQ(send_byte(release[1], 'X'), 0);
	if (wait_result == 0) {
		CHECK_INT_EQ(waitpid(owner, &status, 0), owner);
		CHECK_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
	if (contender_result == 0)
		ltfs_operation_lock_release(&contender);
	close(ready[0]);
	close(daemon_continue[1]);
	close(release[1]);
	CHECK_INT_EQ(ready_byte, 'R');
	CHECK_INT_EQ(wait_result, 0);
	CHECK_INT_EQ(contender_result, LTFS_OPERATION_LOCK_BUSY);
	return 0;
}

static int test_one_of_one_hundred_concurrent_acquisitions_wins(void)
{
	struct lock_fixture fixture;
	struct acquire_context contexts[CONCURRENT_ACQUIRERS];
	pthread_t threads[CONCURRENT_ACQUIRERS];
	pthread_barrier_t barrier;
	int winners = 0;
	int winner = -1;
	int i;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(pthread_barrier_init(&barrier, NULL,
		CONCURRENT_ACQUIRERS + 1), 0);
	memset(contexts, 0, sizeof(contexts));
	for (i = 0; i < CONCURRENT_ACQUIRERS; ++i) {
		contexts[i].identity = &fixture.identity;
		contexts[i].barrier = &barrier;
		CHECK_INT_EQ(pthread_create(&threads[i], NULL,
			concurrent_acquire, &contexts[i]), 0);
	}
	pthread_barrier_wait(&barrier);
	for (i = 0; i < CONCURRENT_ACQUIRERS; ++i) {
		CHECK_INT_EQ(pthread_join(threads[i], NULL), 0);
		if (contexts[i].result == 0) {
			++winners;
			winner = i;
		} else
			CHECK_INT_EQ(contexts[i].result, LTFS_OPERATION_LOCK_BUSY);
	}
	CHECK_INT_EQ(winners, 1);
	ltfs_operation_lock_release(&contexts[winner].lock);
	CHECK_INT_EQ(pthread_barrier_destroy(&barrier), 0);
	return 0;
}

int main(void)
{
	if (test_first_acquisition_live_conflict_and_owned_release() != 0)
		return 1;
	if (test_read_only_info_class_is_serialized() != 0)
		return 1;
	if (test_pid_reuse_and_previous_boot_are_proven_stale() != 0)
		return 1;
	if (test_corrupt_and_wrong_target_records_fail_closed() != 0)
		return 1;
	if (test_release_refuses_changed_ownership() != 0)
		return 1;
	if (test_one_of_one_hundred_concurrent_acquisitions_wins() != 0)
		return 1;
	if (test_forced_foreground_keeps_recorded_owner_live() != 0)
		return 1;
	return 0;
}
