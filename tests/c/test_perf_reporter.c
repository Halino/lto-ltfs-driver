/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "perf_reporter.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define OP_ID "00000000-0000-4000-8000-000000000001"

static uint64_t now_ns;

static uint64_t fake_now(void *context)
{
	(void)context;
	return now_ns;
}

static int sample_runtime(void *context, struct ltfs_perf_runtime_sample *sample)
{
	(void)context;
	sample->queue_fill_bytes = 8192;
	sample->resident_bytes = 64 * 1024 * 1024;
	sample->retry_count = 3;
	sample->buffer_underrun_count = 2;
	return 0;
}

int main(void)
{
	struct ltfs_perf_config config = {
		.monotonic_ns = fake_now,
		.memory_ceiling_bytes = LTFS_PERF_MAX_MEMORY_CEILING,
	};
	struct ltfs_perf_reporter reporter;
	struct ltfs_perf_counters counters;
	struct ltfs_event_sink sink;
	bool emit_due = false;
	char output[2048] = { 0 };
	int pipefd[2];
	ssize_t length;

	CHECK_INT_EQ(pipe(pipefd), 0);
	CHECK_INT_EQ(ltfs_perf_init(&counters, &config), 0);
	CHECK_INT_EQ(ltfs_events_init(&sink, pipefd[1], OP_ID), 0);
	CHECK_INT_EQ(ltfs_perf_reporter_init(&reporter, &counters, &sink,
		sample_runtime, NULL), 0);
	CHECK_INT_EQ(ltfs_perf_record_file(&counters, 77), 0);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 4096, 0, 0,
		&emit_due), 0);
	CHECK_TRUE(emit_due);
	now_ns = 1000000000ULL;
	CHECK_INT_EQ(ltfs_perf_reporter_notify(&reporter), 0);
	ltfs_perf_reporter_stop(&reporter);
	CHECK_INT_EQ(ltfs_perf_reporter_notify(&reporter), 0);
	ltfs_perf_reporter_destroy(&reporter);
	length = read(pipefd[0], output, sizeof(output) - 1);
	CHECK_TRUE(length > 0);
	CHECK_TRUE(strstr(output, "\"bytes_done\":4096") != NULL);
	CHECK_TRUE(strstr(output, "\"files_done\":1") != NULL);
	CHECK_TRUE(strstr(output, "\"queue_fill_bytes\":8192") != NULL);
	CHECK_TRUE(strstr(output, "\"retry_count\":3") != NULL);
	CHECK_TRUE(strstr(output, "\"buffer_underrun_count\":2") != NULL);
	ltfs_events_close(&sink);
	ltfs_perf_destroy(&counters);
	close(pipefd[0]);
	close(pipefd[1]);
	return 0;
}
