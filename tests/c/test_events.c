/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "ltfs_events.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define OP_ID "00000000-0000-4000-8000-000000000001"

enum write_fault {
	WRITE_FAULT_NONE,
	WRITE_FAULT_EINTR,
	WRITE_FAULT_SHORT,
	WRITE_FAULT_PARTIAL_EAGAIN,
	WRITE_FAULT_BLOCK,
};

static enum write_fault active_fault;
static int fault_fd = -1;
static unsigned int fault_calls;
static pthread_mutex_t fault_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fault_condition = PTHREAD_COND_INITIALIZER;
static bool block_entered;
static bool block_release;
static bool measure_realloc;
static unsigned int measured_realloc_calls;

void *__real_realloc(void *pointer, size_t size);

void *__wrap_realloc(void *pointer, size_t size)
{
	if (measure_realloc)
		++measured_realloc_calls;
	return __real_realloc(pointer, size);
}

ssize_t test_event_write(int fd, const void *buffer, size_t count)
{
	if (fd == fault_fd) {
		++fault_calls;
		if (active_fault == WRITE_FAULT_EINTR && fault_calls == 1) {
			errno = EINTR;
			return -1;
		}
		if (active_fault == WRITE_FAULT_SHORT && count > 7)
			count = 7;
		if (active_fault == WRITE_FAULT_PARTIAL_EAGAIN) {
			if (fault_calls == 1)
				count = count > 7 ? 7 : count;
			else {
				errno = EAGAIN;
				return -1;
			}
		}
		if (active_fault == WRITE_FAULT_BLOCK) {
			pthread_mutex_lock(&fault_mutex);
			block_entered = true;
			pthread_cond_broadcast(&fault_condition);
			while (!block_release)
				pthread_cond_wait(&fault_condition, &fault_mutex);
			pthread_mutex_unlock(&fault_mutex);
		}
	}
	return write(fd, buffer, count);
}

static int wait_for_child(pid_t child, unsigned int timeout_ms)
{
	struct timespec delay = { 0, 1000000L };
	unsigned int elapsed;
	int status;

	for (elapsed = 0; elapsed < timeout_ms; ++elapsed) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
		if (result < 0)
			return 1;
		nanosleep(&delay, NULL);
	}
	kill(child, SIGKILL);
	waitpid(child, &status, 0);
	return 124;
}

static int fill_pipe_without_changing_final_flags(int fd)
{
	char block[4096] = { 0 };
	int flags = fcntl(fd, F_GETFL);

	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	while (write(fd, block, sizeof(block)) > 0)
		;
	if (errno != EAGAIN && errno != EWOULDBLOCK)
		return -1;
	return fcntl(fd, F_SETFL, flags);
}

static struct ltfs_event progress_event(const char *device_serial)
{
	return (struct ltfs_event) {
		.volume_uuid = "11111111-1111-4111-8111-111111111111",
		.prior_generation_valid = true,
		.prior_generation = UINT64_C(17),
		.new_generation_valid = true,
		.new_generation = UINT64_C(18),
		.phase = LTFS_PHASE_WRITING_INDEX,
		.status = LTFS_EVENT_PROGRESS,
		.bytes_done = UINT64_C(1048576),
		.bytes_total_valid = true,
		.bytes_total = UINT64_C(4194304),
		.files_done = UINT64_C(120),
		.files_total_valid = true,
		.files_total = UINT64_C(120),
		.rate_bytes_per_second = UINT64_C(777),
		.queue_fill_bytes = UINT64_C(8192),
		.buffer_underrun_count = UINT64_C(2),
		.retry_count = UINT64_C(3),
		.memory_high_water_bytes = UINT64_C(67108864),
		.telemetry_overflowed = false,
		.phase_elapsed_ns = UINT64_C(99),
		.index_progress_valid = true,
		.index_done = UINT64_C(1),
		.index_total = UINT64_C(4),
		.eta_seconds_valid = false,
		.device_serial = device_serial,
		.message_code = "index.write.progress",
	};
}

static char *read_one_line(int fd)
{
	size_t capacity = 256;
	size_t length = 0;
	char *line = malloc(capacity);
	char byte;
	ssize_t count;

	if (!line)
		return NULL;
	for (;;) {
		count = read(fd, &byte, 1);
		if (count != 1) {
			free(line);
			return NULL;
		}
		if (length + 2 > capacity) {
			char *grown;
			capacity *= 2;
			grown = realloc(line, capacity);
			if (!grown) {
				free(line);
				return NULL;
			}
			line = grown;
		}
		line[length++] = byte;
		if (byte == '\n')
			break;
	}
	line[length] = '\0';
	return line;
}

static int test_order_and_required_fields(void)
{
	static const char prefix[] =
		"{\"schema\":1,\"operation_id\":\"" OP_ID
		"\",\"volume_uuid\":\"11111111-1111-4111-8111-111111111111\""
		",\"prior_generation\":17,\"new_generation\":18,\"seq\":1,";
	static const char *ordered_keys[] = {
		"\"monotonic_ns\":", "\"wall_time\":", "\"phase\":",
		"\"status\":", "\"result\":", "\"device_close_result\":",
		"\"bytes_done\":", "\"bytes_total\":",
		"\"files_done\":", "\"files_total\":",
		"\"rate_bytes_per_second\":", "\"queue_fill_bytes\":",
		"\"buffer_underrun_count\":", "\"retry_count\":",
		"\"memory_high_water_bytes\":", "\"telemetry_overflowed\":",
		"\"phase_elapsed_ns\":",
		"\"index_done\":", "\"index_total\":", "\"eta_seconds\":",
		"\"device_serial\":", "\"message_code\":"
	};
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	const char *position;
	char *line;
	int pipefd[2];
	size_t i;

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	measured_realloc_calls = 0;
	measure_realloc = true;
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	measure_realloc = false;
	CHECK_INT_EQ(measured_realloc_calls, 0);
	line = read_one_line(pipefd[0]);
	CHECK_TRUE(line != NULL);
	CHECK_TRUE(strncmp(line, prefix, sizeof(prefix) - 1) == 0);
	position = line;
	for (i = 0; i < sizeof(ordered_keys) / sizeof(ordered_keys[0]); ++i) {
		position = strstr(position, ordered_keys[i]);
		CHECK_TRUE(position != NULL);
		position += strlen(ordered_keys[i]);
	}
	CHECK_TRUE(strstr(line, "\"phase\":\"WRITING_INDEX\"") != NULL);
	CHECK_TRUE(strstr(line, "\"status\":\"progress\"") != NULL);
	CHECK_TRUE(strstr(line, "\"device_close_result\":null") != NULL);
	CHECK_TRUE(strstr(line, "\"eta_seconds\":null") != NULL);
	CHECK_TRUE(strstr(line, "\"queue_fill_bytes\":8192") != NULL);
	CHECK_TRUE(strstr(line, "\"buffer_underrun_count\":2") != NULL);
	CHECK_TRUE(strstr(line, "\"retry_count\":3") != NULL);
	CHECK_TRUE(strstr(line, "\"memory_high_water_bytes\":67108864") != NULL);
	CHECK_TRUE(strstr(line, "\"telemetry_overflowed\":false") != NULL);
	CHECK_TRUE(line[strlen(line) - 1] == '\n');
	free(line);
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_device_close_result_is_serialized(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	char *line;
	int pipefd[2];

	event.device_close_result_valid = true;
	event.device_close_result = -5;
	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	line = read_one_line(pipefd[0]);
	CHECK_TRUE(line != NULL);
	CHECK_TRUE(strstr(line, "\"device_close_result\":-5") != NULL);
	free(line);
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_json_escaping(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event("drive\"\\\b\f\n\r\t\001");
	char *line;
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	line = read_one_line(pipefd[0]);
	CHECK_TRUE(line != NULL);
	CHECK_TRUE(strstr(line,
		"\"device_serial\":\"drive\\\"\\\\\\b\\f\\n\\r\\t\\u0001\"") != NULL);
	free(line);
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_two_events_are_sequenced(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	char *first;
	char *second;
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	first = read_one_line(pipefd[0]);
	second = read_one_line(pipefd[0]);
	CHECK_TRUE(first != NULL && second != NULL);
	CHECK_TRUE(strstr(first, "\"seq\":1,") != NULL);
	CHECK_TRUE(strstr(second, "\"seq\":2,") != NULL);
	CHECK_TRUE(strchr(first, '\n') == first + strlen(first) - 1);
	CHECK_TRUE(strchr(second, '\n') == second + strlen(second) - 1);
	free(first);
	free(second);
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_full_pipe_disables_sink_without_blocking(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	pid_t child;
	int pipefd[2];
	int child_result;

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(fill_pipe_without_changing_final_flags(pipefd[1]), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	child = fork();
	CHECK_TRUE(child >= 0);
	if (child == 0) {
		int first = ltfs_event_emit(&sink, &event);
		int second = ltfs_event_emit(&sink, &event);
		_exit(first == -EAGAIN && second == 0 && sink.seq == 0 ? 0 : 1);
	}
	child_result = wait_for_child(child, 2000);
	if (child_result == 124)
		fprintf(stderr, "event emit blocked on a full pipe\n");
	CHECK_INT_EQ(child_result, 0);
	CHECK_TRUE((fcntl(pipefd[1], F_GETFL) & O_NONBLOCK) != 0);
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

struct emit_thread_args {
	struct ltfs_event_sink *sink;
	struct ltfs_event event;
	int result;
};

struct close_thread_args {
	struct ltfs_event_sink *sink;
	bool started;
	bool done;
};

static void *emit_thread(void *opaque)
{
	struct emit_thread_args *args = opaque;
	args->result = ltfs_event_emit(args->sink, &args->event);
	return NULL;
}

static void *close_thread(void *opaque)
{
	struct close_thread_args *args = opaque;

	pthread_mutex_lock(&fault_mutex);
	args->started = true;
	pthread_cond_broadcast(&fault_condition);
	pthread_mutex_unlock(&fault_mutex);
	ltfs_events_close(args->sink);
	pthread_mutex_lock(&fault_mutex);
	args->done = true;
	pthread_cond_broadcast(&fault_condition);
	pthread_mutex_unlock(&fault_mutex);
	return NULL;
}

static int test_close_serializes_with_emit_and_late_emit_is_safe(void)
{
	struct ltfs_event_sink sink;
	struct emit_thread_args emit_args;
	struct close_thread_args close_args = { 0 };
	pthread_t emitter;
	pthread_t closer;
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	emit_args.sink = &sink;
	emit_args.event = progress_event(NULL);
	emit_args.result = -1;
	close_args.sink = &sink;
	active_fault = WRITE_FAULT_BLOCK;
	fault_fd = sink.fd;
	block_entered = false;
	block_release = false;
	CHECK_INT_EQ(pthread_create(&emitter, NULL, emit_thread, &emit_args), 0);
	pthread_mutex_lock(&fault_mutex);
	while (!block_entered)
		pthread_cond_wait(&fault_condition, &fault_mutex);
	pthread_mutex_unlock(&fault_mutex);
	CHECK_INT_EQ(pthread_create(&closer, NULL, close_thread, &close_args), 0);
	pthread_mutex_lock(&fault_mutex);
	while (!close_args.started)
		pthread_cond_wait(&fault_condition, &fault_mutex);
	CHECK_TRUE(!close_args.done);
	block_release = true;
	pthread_cond_broadcast(&fault_condition);
	pthread_mutex_unlock(&fault_mutex);
	CHECK_INT_EQ(pthread_join(emitter, NULL), 0);
	CHECK_INT_EQ(pthread_join(closer, NULL), 0);
	CHECK_INT_EQ(emit_args.result, 0);
	CHECK_TRUE(close_args.done);
	active_fault = WRITE_FAULT_NONE;
	fault_fd = -1;
	CHECK_INT_EQ(ltfs_event_emit(&sink, &emit_args.event), 0);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_retries_interrupted_and_short_writes(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	char *line;
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	fault_fd = sink.fd;
	active_fault = WRITE_FAULT_EINTR;
	fault_calls = 0;
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	CHECK_TRUE(fault_calls >= 2);
	line = read_one_line(pipefd[0]);
	CHECK_TRUE(line != NULL);
	free(line);

	active_fault = WRITE_FAULT_SHORT;
	fault_calls = 0;
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	CHECK_TRUE(fault_calls > 2);
	line = read_one_line(pipefd[0]);
	CHECK_TRUE(line != NULL);
	CHECK_TRUE(strstr(line, "\"seq\":2,") != NULL);
	free(line);

	active_fault = WRITE_FAULT_NONE;
	fault_fd = -1;
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_partial_then_eagain_disables_sink(void)
{
	static const char expected_partial[] = "{\"schem";
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	char partial[sizeof(expected_partial)] = { 0 };
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	active_fault = WRITE_FAULT_PARTIAL_EAGAIN;
	fault_fd = sink.fd;
	fault_calls = 0;
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), -EAGAIN);
	CHECK_TRUE(sink.fd == -1);
	CHECK_TRUE(sink.seq == 0);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	CHECK_INT_EQ(read(pipefd[0], partial, sizeof(partial) - 1), 7);
	CHECK_STR_EQ(partial, expected_partial);
	active_fault = WRITE_FAULT_NONE;
	fault_fd = -1;
	ltfs_events_close(&sink);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}

static int test_closed_reader_returns_epipe(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	close(pipefd[0]);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), -EPIPE);
	ltfs_events_close(&sink);
	close(pipefd[1]);
	return 0;
}

static int test_disabled_sink_is_a_noop(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);

	CHECK_INT_EQ(ltfs_events_init(&sink, -1, OP_ID), 0);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	CHECK_TRUE(sink.seq == 0);
	ltfs_events_close(&sink);
	return 0;
}

static int test_sink_duplicates_fd_and_copies_operation_id(void)
{
	char operation_id[] = OP_ID;
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);
	char *line;
	int pipefd[2];

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], operation_id), 0);
	CHECK_TRUE(sink.fd != pipefd[1]);
	CHECK_TRUE((fcntl(sink.fd, F_GETFD) & FD_CLOEXEC) != 0);
	operation_id[0] = 'f';
	close(pipefd[1]);
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	line = read_one_line(pipefd[0]);
	CHECK_TRUE(line != NULL);
	CHECK_TRUE(strstr(line, "\"operation_id\":\"" OP_ID "\"") != NULL);
	free(line);
	ltfs_events_close(&sink);
	close(pipefd[0]);
	return 0;
}

static int test_volume_identity_must_be_schema_valid(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event(NULL);

	CHECK_INT_EQ(ltfs_events_init(&sink, -1, OP_ID), 0);
	event.volume_uuid = "not-a-uuid";
	CHECK_INT_EQ(ltfs_event_emit(&sink, &event), -EINVAL);
	ltfs_events_close(&sink);
	return 0;
}

static int emit_schema_fixtures(void)
{
	struct ltfs_event_sink sink;
	struct ltfs_event event = progress_event("synthetic\"drive\n01");
	char *line;
	int pipefd[2];
	int i;

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	for (i = 0; i < 2; ++i)
		CHECK_INT_EQ(ltfs_event_emit(&sink, &event), 0);
	ltfs_events_close(&sink);
	close(pipefd[1]);
	for (i = 0; i < 2; ++i) {
		line = read_one_line(pipefd[0]);
		CHECK_TRUE(line != NULL);
		CHECK_TRUE(fputs(line, stdout) >= 0);
		free(line);
	}
	close(pipefd[0]);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--emit-fixtures") == 0)
		return emit_schema_fixtures();
	if (argc == 2 && strcmp(argv[1], "--test-nonblocking") == 0)
		return test_full_pipe_disables_sink_without_blocking();
	if (argc == 2 && strcmp(argv[1], "--test-concurrent-close") == 0)
		return test_close_serializes_with_emit_and_late_emit_is_safe();
	if (argc != 1)
		return 2;
	if (test_order_and_required_fields() != 0)
		return 1;
	if (test_device_close_result_is_serialized() != 0)
		return 1;
	if (test_json_escaping() != 0)
		return 1;
	if (test_two_events_are_sequenced() != 0)
		return 1;
	if (test_full_pipe_disables_sink_without_blocking() != 0)
		return 1;
	if (test_close_serializes_with_emit_and_late_emit_is_safe() != 0)
		return 1;
	if (test_retries_interrupted_and_short_writes() != 0)
		return 1;
	if (test_partial_then_eagain_disables_sink() != 0)
		return 1;
	if (test_closed_reader_returns_epipe() != 0)
		return 1;
	if (test_disabled_sink_is_a_noop() != 0)
		return 1;
	if (test_sink_duplicates_fd_and_copies_operation_id() != 0)
		return 1;
	if (test_volume_identity_must_be_schema_valid() != 0)
		return 1;
	return 0;
}
