/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "operation_control.h"

#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OP_ID "00000000-0000-4000-8000-000000000001"

struct add_context {
	struct ltfs_interval_control *control;
	uint64_t bytes;
};

struct wait_context {
	struct ltfs_interval_control *control;
	bool claimed;
};

struct cancel_context {
	int calls;
};

struct commit_context {
	int flush_result;
	int index_result;
	int now_result;
	uint64_t now_ns;
	int flush_calls;
	int index_calls;
	int now_calls;
};

static void cancel_operation(void *opaque)
{
	struct cancel_context *context = opaque;
	++context->calls;
}

static int fake_flush(void *opaque)
{
	struct commit_context *context = opaque;
	++context->flush_calls;
	return context->flush_result;
}

static int fake_index(void *opaque)
{
	struct commit_context *context = opaque;
	++context->index_calls;
	return context->index_result;
}

static int fake_now(void *opaque, uint64_t *now_ns)
{
	struct commit_context *context = opaque;
	++context->now_calls;
	*now_ns = context->now_ns;
	return context->now_result;
}

static void *add_bytes(void *opaque)
{
	struct add_context *context = opaque;
	ltfs_interval_control_add_bytes(context->control, context->bytes);
	return NULL;
}

static void *wait_for_claim(void *opaque)
{
	struct wait_context *context = opaque;
	context->claimed = ltfs_interval_control_wait_claim(context->control);
	return NULL;
}

static int test_cli_matrix(void)
{
	struct ltfs_operation_options options;
	char fd_option[64];
	char *valid_args[] = {
		"ltfs", fd_option, "--event-schema=1", "--operation-id=" OP_ID,
		"--index-policy=unmount", NULL
	};
	char *valid_interval_args[] = {
		"ltfs", "--index-policy=interval", "--index-interval-bytes=1024",
		"--index-interval-seconds=5", NULL
	};
	char *valid_legacy_args[] = { "ltfs", "-o", "sync_type=time@5", NULL };
	const char *invalid_options[][5] = {
		{"ltfs", "--event-fd", NULL},
		{"ltfs", "--event-fd=abc", NULL},
		{"ltfs", "--event-fd=-1", NULL},
		{"ltfs", "--event-fd=2", NULL},
		{"ltfs", "--event-schema=0", NULL},
		{"ltfs", "--event-schema=2", NULL},
		{"ltfs", "--operation-id=not-a-uuid", NULL},
		{"ltfs", "--index-policy=unknown", NULL},
		{"ltfs", "--index-policy=interval", "--index-interval-bytes=1", NULL},
		{"ltfs", "--index-policy=interval", "--index-interval-seconds=1", NULL},
		{"ltfs", "--index-policy=interval", "--index-interval-bytes=0", "--index-interval-seconds=1", NULL},
		{"ltfs", "--index-policy=unmount", "--index-interval-bytes=1", NULL},
		{"ltfs", "--index-policy=unmount", "-osync_type=unmount", NULL},
	};
	int pipefd[2];
	int sockets[2];
	char path[] = "/tmp/ltfs-operation-control.XXXXXX";
	int regular_fd;
	size_t i;

	for (i = 0; i < sizeof(invalid_options) / sizeof(invalid_options[0]); ++i)
		CHECK_TRUE(ltfs_operation_options_parse(5,
			(char *const *)invalid_options[i], &options) != LTFS_OPERATION_OPTIONS_OK);
	CHECK_INT_EQ(ltfs_operation_options_parse(4, valid_interval_args, &options),
		LTFS_OPERATION_OPTIONS_OK);
	CHECK_INT_EQ(options.index_policy, LTFS_INDEX_POLICY_INTERVAL);
	CHECK_INT_EQ(options.interval_bytes, 1024);
	CHECK_INT_EQ(options.interval_seconds, 5);
	CHECK_INT_EQ(ltfs_operation_options_parse(3, valid_legacy_args, &options),
		LTFS_OPERATION_OPTIONS_OK);
	CHECK_INT_EQ(options.index_policy, LTFS_INDEX_POLICY_LEGACY);

	CHECK_INT_EQ(pipe(pipefd), 0);
	snprintf(fd_option, sizeof(fd_option), "--event-fd=%d", pipefd[1]);
	CHECK_INT_EQ(ltfs_operation_options_parse(5, valid_args, &options),
		LTFS_OPERATION_OPTIONS_OK);
	CHECK_TRUE(options.events_enabled);
	CHECK_INT_EQ(options.event_fd, pipefd[1]);
	CHECK_INT_EQ(options.index_policy, LTFS_INDEX_POLICY_UNMOUNT);
	close(pipefd[0]);
	close(pipefd[1]);

	CHECK_INT_EQ(pipe(pipefd), 0);
	snprintf(fd_option, sizeof(fd_option), "--event-fd=%d", pipefd[0]);
	CHECK_TRUE(ltfs_operation_options_parse(5, valid_args, &options) !=
		LTFS_OPERATION_OPTIONS_OK);
	close(pipefd[0]);
	close(pipefd[1]);

	regular_fd = mkstemp(path);
	CHECK_TRUE(regular_fd >= 0);
	snprintf(fd_option, sizeof(fd_option), "--event-fd=%d", regular_fd);
	CHECK_TRUE(ltfs_operation_options_parse(5, valid_args, &options) !=
		LTFS_OPERATION_OPTIONS_OK);
	close(regular_fd);
	unlink(path);

	CHECK_INT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
	snprintf(fd_option, sizeof(fd_option), "--event-fd=%d", sockets[0]);
	CHECK_TRUE(ltfs_operation_options_parse(5, valid_args, &options) !=
		LTFS_OPERATION_OPTIONS_OK);
	close(sockets[0]);
	close(sockets[1]);
	return 0;
}

static int test_operational_result_survives_cleanup(void)
{
	int result;

	CHECK_INT_EQ(ltfs_operation_result_preserve(-17, 0), -17);
	CHECK_INT_EQ(ltfs_operation_result_preserve(-17, -23), -17);
	CHECK_INT_EQ(ltfs_operation_result_preserve(0, -23), -23);
	CHECK_INT_EQ(ltfs_operation_result_preserve(0, 0), 0);

	/* Model the exact fuse/mount-or-READY/policy/cleanup precedence in main. */
	result = ltfs_operation_result_preserve(0, -EIO);
	result = ltfs_operation_result_preserve(0, result);
	result = ltfs_operation_result_preserve(result, -EINTR);
	CHECK_INT_EQ(result, -EIO);

	result = ltfs_operation_result_preserve(-ENODEV, 0);
	result = ltfs_operation_result_preserve(0, result);
	result = ltfs_operation_result_preserve(result, -EINTR);
	CHECK_INT_EQ(result, -ENODEV);

	result = ltfs_operation_result_preserve(0, 0);
	result = ltfs_operation_result_preserve(-ENOSPC, result);
	result = ltfs_operation_result_preserve(result, -EINTR);
	CHECK_INT_EQ(result, -ENOSPC);
	return 0;
}

static int test_terminal_policy_failure_is_durable(void)
{
	struct ltfs_operation_failure failure;
	struct cancel_context cancelled = { 0 };
	struct ltfs_interval_control *control = NULL;
	const uint64_t start = UINT64_C(10000000000);

	ltfs_operation_failure_init(&failure, cancel_operation, &cancelled);
	CHECK_INT_EQ(ltfs_operation_failure_result(&failure), 0);
	CHECK_INT_EQ(ltfs_operation_write_gate(&failure), 0);
	CHECK_TRUE(ltfs_operation_failure_record(&failure, -EIO));
	CHECK_INT_EQ(cancelled.calls, 1);
	CHECK_INT_EQ(ltfs_operation_failure_result(&failure), -EIO);
	CHECK_INT_EQ(ltfs_operation_write_gate(&failure), -EIO);
	CHECK_TRUE(!ltfs_operation_failure_record(&failure, -ENOSPC));
	CHECK_INT_EQ(cancelled.calls, 1);
	CHECK_INT_EQ(ltfs_operation_failure_result(&failure), -EIO);

	CHECK_INT_EQ(ltfs_interval_control_create(100, 5, start, &control), 0);
	ltfs_interval_control_add_bytes(control, 100);
	CHECK_TRUE(ltfs_interval_control_claim_at(control, start));
	ltfs_interval_control_complete_at(control, -EIO, start + 1);
	CHECK_INT_EQ(ltfs_interval_control_error(control), -EIO);
	ltfs_interval_control_add_bytes(control, 100);
	CHECK_TRUE(!ltfs_interval_control_claim_at(control, start + UINT64_C(10000000000)));
	ltfs_interval_control_destroy(control);
	return 0;
}

static int test_interval_commit_fault_injection(void)
{
	struct ltfs_operation_failure failure;
	struct cancel_context cancelled = { 0 };
	struct ltfs_interval_control *control = NULL;
	struct commit_context commit = {
		.flush_result = -EIO,
		.index_result = 0,
		.now_result = 0,
		.now_ns = UINT64_C(20000000000),
	};
	const uint64_t start = UINT64_C(10000000000);

	ltfs_operation_failure_init(&failure, cancel_operation, &cancelled);
	CHECK_INT_EQ(ltfs_interval_control_create(100, 5, start, &control), 0);
	ltfs_interval_control_add_bytes(control, 100);
	CHECK_TRUE(ltfs_interval_control_claim_at(control, start));
	CHECK_INT_EQ(ltfs_interval_commit_execute(control, &failure,
		fake_flush, fake_index, fake_now, &commit), -EIO);
	CHECK_INT_EQ(commit.flush_calls, 1);
	CHECK_INT_EQ(commit.index_calls, 0);
	CHECK_INT_EQ(commit.now_calls, 0);
	CHECK_INT_EQ(cancelled.calls, 1);
	CHECK_INT_EQ(ltfs_operation_failure_result(&failure), -EIO);
	ltfs_interval_control_destroy(control);

	memset(&commit, 0, sizeof(commit));
	commit.index_result = -ENOSPC;
	commit.now_ns = UINT64_C(20000000000);
	cancelled.calls = 0;
	ltfs_operation_failure_init(&failure, cancel_operation, &cancelled);
	CHECK_INT_EQ(ltfs_interval_control_create(100, 5, start, &control), 0);
	ltfs_interval_control_add_bytes(control, 100);
	CHECK_TRUE(ltfs_interval_control_claim_at(control, start));
	CHECK_INT_EQ(ltfs_interval_commit_execute(control, &failure,
		fake_flush, fake_index, fake_now, &commit), -ENOSPC);
	CHECK_INT_EQ(commit.flush_calls, 1);
	CHECK_INT_EQ(commit.index_calls, 1);
	CHECK_INT_EQ(commit.now_calls, 0);
	CHECK_INT_EQ(cancelled.calls, 1);
	CHECK_INT_EQ(ltfs_operation_failure_result(&failure), -ENOSPC);
	ltfs_interval_control_destroy(control);
	return 0;
}

static int test_interval_boundaries_and_atomic_reset(void)
{
	struct ltfs_interval_control *control = NULL;
	const uint64_t start = UINT64_C(10000000000);
	pthread_t adders[4];
	struct add_context contexts[4];
	int i;

	CHECK_INT_EQ(ltfs_interval_control_create(100, 5, start, &control), 0);
	CHECK_TRUE(!ltfs_interval_control_claim_at(control,
		start + UINT64_C(4999999999)));
	CHECK_TRUE(ltfs_interval_control_claim_at(control,
		start + UINT64_C(5000000000)));
	CHECK_TRUE(!ltfs_interval_control_claim_at(control,
		start + UINT64_C(5000000000)));
	ltfs_interval_control_complete_at(control, 0,
		start + UINT64_C(5000000000));

	for (i = 0; i < 4; ++i) {
		contexts[i].control = control;
		contexts[i].bytes = 25;
		CHECK_INT_EQ(pthread_create(&adders[i], NULL, add_bytes, &contexts[i]), 0);
	}
	for (i = 0; i < 4; ++i)
		CHECK_INT_EQ(pthread_join(adders[i], NULL), 0);
	CHECK_TRUE(ltfs_interval_control_claim_at(control,
		start + UINT64_C(5000000001)));
	CHECK_TRUE(!ltfs_interval_control_claim_at(control,
		start + UINT64_C(5000000001)));

	ltfs_interval_control_add_bytes(control, 100);
	CHECK_TRUE(!ltfs_interval_control_claim_at(control,
		start + UINT64_C(5000000001)));
	ltfs_interval_control_complete_at(control, 0,
		start + UINT64_C(5000000001));
	CHECK_TRUE(ltfs_interval_control_claim_at(control,
		start + UINT64_C(5000000001)));
	ltfs_interval_control_complete_at(control, 0,
		start + UINT64_C(5000000001));
	ltfs_interval_control_destroy(control);
	return 0;
}

static int test_time_deadline_rearms_after_long_commit(void)
{
	struct ltfs_interval_control *control = NULL;
	const uint64_t start = UINT64_C(10000000000);
	const uint64_t second = UINT64_C(1000000000);

	CHECK_INT_EQ(ltfs_interval_control_create(100, 5, start, &control), 0);
	CHECK_TRUE(ltfs_interval_control_claim_at(control, start + 5 * second));
	ltfs_interval_control_complete_at(control, 0, start + 20 * second);
	CHECK_TRUE(!ltfs_interval_control_claim_at(control,
		start + 25 * second - 1));
	CHECK_TRUE(ltfs_interval_control_claim_at(control, start + 25 * second));
	ltfs_interval_control_complete_at(control, 0, start + 25 * second);
	ltfs_interval_control_destroy(control);
	return 0;
}

static int test_byte_threshold_wakes_waiter(void)
{
	struct ltfs_interval_control *control = NULL;
	struct wait_context context;
	pthread_t waiter;
	struct timespec join_deadline;
	struct timespec now;
	uint64_t now_ns;

	CHECK_INT_EQ(clock_gettime(CLOCK_MONOTONIC, &now), 0);
	now_ns = (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
	CHECK_INT_EQ(ltfs_interval_control_create(64, 3600, now_ns, &control), 0);
	context.control = control;
	context.claimed = false;
	CHECK_INT_EQ(pthread_create(&waiter, NULL, wait_for_claim, &context), 0);
	ltfs_interval_control_add_bytes(control, 63);
	CHECK_INT_EQ(clock_gettime(CLOCK_REALTIME, &join_deadline), 0);
	join_deadline.tv_nsec += 10000000L;
	if (join_deadline.tv_nsec >= 1000000000L) {
		++join_deadline.tv_sec;
		join_deadline.tv_nsec -= 1000000000L;
	}
	CHECK_INT_EQ(pthread_timedjoin_np(waiter, NULL, &join_deadline), ETIMEDOUT);
	ltfs_interval_control_add_bytes(control, 1);
	CHECK_INT_EQ(pthread_join(waiter, NULL), 0);
	CHECK_TRUE(context.claimed);
	ltfs_interval_control_complete_at(control, 0, 0);
	ltfs_interval_control_stop(control);
	ltfs_interval_control_destroy(control);
	return 0;
}

int main(void)
{
	if (test_cli_matrix() != 0)
		return 1;
	if (test_operational_result_survives_cleanup() != 0)
		return 1;
	if (test_terminal_policy_failure_is_durable() != 0)
		return 1;
	if (test_interval_commit_fault_injection() != 0)
		return 1;
	if (test_interval_boundaries_and_atomic_reset() != 0)
		return 1;
	if (test_time_deadline_rearms_after_long_commit() != 0)
		return 1;
	if (test_byte_threshold_wakes_waiter() != 0)
		return 1;
	return 0;
}
