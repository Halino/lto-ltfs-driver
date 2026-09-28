/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "perf_counters.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#define NS_PER_SECOND 1000000000ULL
#define MIB (1024ULL * 1024ULL)

struct fake_clock {
	uint64_t now_ns;
};

static uint64_t fake_now(void *context)
{
	return ((struct fake_clock *)context)->now_ns;
}

static int init_counters(struct ltfs_perf_counters *counters,
	struct fake_clock *clock)
{
	struct ltfs_perf_config config = {
		.monotonic_ns = fake_now,
		.clock_context = clock,
		.memory_ceiling_bytes = 512 * MIB,
	};
	return ltfs_perf_init(counters, &config);
}

static int test_windows_and_idle_intervals(void)
{
	struct fake_clock clock = {0};
	struct ltfs_perf_counters counters;
	struct ltfs_perf_snapshot snapshot;
	bool emit_due = false;
	CHECK_INT_EQ(init_counters(&counters, &clock), 0);

	CHECK_INT_EQ(ltfs_perf_record_file(&counters, 1), 0);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1000, 20, 30 * MIB,
		&emit_due), 0);
	CHECK_TRUE(emit_due);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_TRUE(!snapshot.rate_valid);
	CHECK_TRUE(!snapshot.average_rate_valid);

	clock.now_ns = NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_TRUE(snapshot.rate_valid);
	CHECK_INT_EQ(snapshot.rate_bytes_per_second, 1000);
	CHECK_INT_EQ(snapshot.average_bytes_per_second, 1000);

	clock.now_ns = 10 * NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_record_file(&counters, 2), 0);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 9000, 40, 80 * MIB,
		&emit_due), 0);
	CHECK_TRUE(emit_due);
	clock.now_ns = 11 * NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 10, &snapshot), 0);
	CHECK_INT_EQ(snapshot.rate_bytes_per_second, 900);
	CHECK_INT_EQ(snapshot.average_bytes_per_second, 909);
	CHECK_INT_EQ(snapshot.total_bytes, 10000);
	CHECK_INT_EQ(snapshot.total_files, 2);
	CHECK_INT_EQ(snapshot.queue_fill_bytes, 40);
	CHECK_INT_EQ(snapshot.memory_high_water_bytes, 80 * MIB);

	clock.now_ns = 61 * NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 60, &snapshot), 0);
	CHECK_INT_EQ(snapshot.rate_bytes_per_second, 150);
	clock.now_ns = 71 * NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 10, &snapshot), 0);
	CHECK_INT_EQ(snapshot.rate_bytes_per_second, 0);
	ltfs_perf_destroy(&counters);
	return 0;
}

static int test_fractional_average_uses_nanoseconds(void)
{
	struct fake_clock clock = {0};
	struct ltfs_perf_counters counters;
	struct ltfs_perf_snapshot snapshot;
	bool emit_due = false;
	CHECK_INT_EQ(init_counters(&counters, &clock), 0);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1000, 0, 0,
		&emit_due), 0);
	clock.now_ns = NS_PER_SECOND + NS_PER_SECOND / 2;
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.average_bytes_per_second, 666);
	ltfs_perf_destroy(&counters);
	return 0;
}

static int test_unique_file_count_is_bounded(void)
{
	struct fake_clock clock = {0};
	struct ltfs_perf_counters counters;
	struct ltfs_perf_snapshot snapshot;
	uint64_t identity;
	CHECK_INT_EQ(init_counters(&counters, &clock), 0);
	CHECK_INT_EQ(ltfs_perf_record_file(&counters, 17), 0);
	CHECK_INT_EQ(ltfs_perf_record_file(&counters, 17), 0);
	CHECK_INT_EQ(ltfs_perf_record_file(&counters, 29), 0);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.total_files, 2);
	CHECK_TRUE(snapshot.total_files_valid);
	for (identity = 1; identity <= LTFS_PERF_FILE_ID_CAPACITY; ++identity)
		CHECK_INT_EQ(ltfs_perf_record_file(&counters, identity), 0);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.total_files, LTFS_PERF_FILE_ID_CAPACITY);
	CHECK_TRUE(snapshot.total_files_valid);
	CHECK_INT_EQ(ltfs_perf_record_file(&counters,
		LTFS_PERF_FILE_ID_CAPACITY + 1), -ENOSPC);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_TRUE(snapshot.file_identity_overflowed);
	CHECK_TRUE(!snapshot.total_files_valid);
	CHECK_INT_EQ(snapshot.total_files, LTFS_PERF_FILE_ID_CAPACITY);
	ltfs_perf_destroy(&counters);
	return 0;
}

static int test_emit_cadence_reset_and_overflow(void)
{
	struct fake_clock clock = {0};
	struct ltfs_perf_counters counters;
	struct ltfs_perf_snapshot snapshot;
	bool emit_due = false;
	CHECK_INT_EQ(init_counters(&counters, &clock), 0);

	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1, 0, 0,
		&emit_due), 0);
	CHECK_TRUE(emit_due);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1, 0, 0,
		&emit_due), 0);
	CHECK_TRUE(!emit_due);
	clock.now_ns = NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1, 0, 0,
		&emit_due), 0);
	CHECK_TRUE(emit_due);

	clock.now_ns = 2 * NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_reset(&counters), 0);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.total_bytes, 0);
	CHECK_TRUE(!snapshot.rate_valid);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, UINT64_MAX,
		0, 0, &emit_due), 0);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1, 0, 0,
		&emit_due), -EOVERFLOW);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.total_bytes, UINT64_MAX);
	CHECK_INT_EQ(snapshot.total_files, 0);
	CHECK_TRUE(snapshot.overflowed);
	ltfs_perf_destroy(&counters);
	return 0;
}

static int test_finalization_and_auxiliary_counters(void)
{
	struct fake_clock clock = {0};
	struct ltfs_perf_counters counters;
	struct ltfs_perf_counters invalid_counters;
	struct ltfs_perf_snapshot snapshot;
	struct ltfs_perf_config invalid = {
		.monotonic_ns = fake_now,
		.clock_context = &clock,
		.memory_ceiling_bytes = 512 * MIB + 1,
	};
	bool emit_due = false;

	CHECK_TRUE(ltfs_perf_init(&invalid_counters, &invalid) < 0);
	CHECK_INT_EQ(init_counters(&counters, &clock), 0);
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 4096, 7, 64 * MIB,
		&emit_due), 0);
	CHECK_INT_EQ(ltfs_perf_record_retry(&counters), 0);
	CHECK_INT_EQ(ltfs_perf_record_retry(&counters), 0);
	CHECK_INT_EQ(ltfs_perf_record_buffer_underrun(&counters), 0);
	CHECK_INT_EQ(ltfs_perf_record_phase_duration(&counters,
		LTFS_PERF_PHASE_UNMOUNT, 123), 0);
	CHECK_INT_EQ(ltfs_perf_set_finalizing(&counters, true), 0);
	clock.now_ns = NS_PER_SECOND;
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_TRUE(!snapshot.rate_valid);
	CHECK_TRUE(!snapshot.average_rate_valid);
	CHECK_INT_EQ(snapshot.retry_count, 2);
	CHECK_INT_EQ(snapshot.buffer_underrun_count, 1);
	CHECK_INT_EQ(snapshot.phase_duration_ns[LTFS_PERF_PHASE_UNMOUNT], 123);
	ltfs_perf_destroy(&counters);
	return 0;
}

static int test_absolute_runtime_samples_do_not_double_count(void)
{
	struct fake_clock clock = { NS_PER_SECOND };
	struct ltfs_perf_counters counters;
	struct ltfs_perf_snapshot snapshot;

	CHECK_INT_EQ(init_counters(&counters, &clock), 0);
	CHECK_INT_EQ(ltfs_perf_update_samples(&counters, 4096, 32 * MIB, 7, 2), 0);
	CHECK_INT_EQ(ltfs_perf_update_samples(&counters, 8192, 40 * MIB, 7, 2), 0);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.queue_fill_bytes, 8192);
	CHECK_INT_EQ(snapshot.memory_high_water_bytes, 40 * MIB);
	CHECK_INT_EQ(snapshot.retry_count, 7);
	CHECK_INT_EQ(snapshot.buffer_underrun_count, 2);
	CHECK_INT_EQ(ltfs_perf_update_samples(&counters, 0, 513 * MIB, 7, 2), -E2BIG);
	ltfs_perf_destroy(&counters);
	return 0;
}

struct writer_args {
	struct ltfs_perf_counters *counters;
	uint64_t identity;
	int result;
};

static void *concurrent_writer(void *opaque)
{
	struct writer_args *args = opaque;
	bool emit_due;
	unsigned int index;

	args->result = ltfs_perf_record_file(args->counters, args->identity);
	for (index = 0; args->result == 0 && index < 1000; ++index)
		args->result = ltfs_perf_record_write(args->counters, 1, 0, 0,
			&emit_due);
	return NULL;
}

static int test_concurrent_updates_are_exact(void)
{
	struct fake_clock clock = { 0 };
	struct ltfs_perf_counters counters;
	struct ltfs_perf_snapshot snapshot;
	struct writer_args args[8];
	pthread_t threads[8];
	size_t index;

	CHECK_INT_EQ(init_counters(&counters, &clock), 0);
	for (index = 0; index < 8; ++index) {
		args[index] = (struct writer_args){ &counters, index + 1, -1 };
		CHECK_INT_EQ(pthread_create(&threads[index], NULL,
			concurrent_writer, &args[index]), 0);
	}
	for (index = 0; index < 8; ++index) {
		CHECK_INT_EQ(pthread_join(threads[index], NULL), 0);
		CHECK_INT_EQ(args[index].result, 0);
	}
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 1, &snapshot), 0);
	CHECK_INT_EQ(snapshot.total_bytes, 8000);
	CHECK_INT_EQ(snapshot.total_files, 8);
	ltfs_perf_destroy(&counters);
	return 0;
}

int main(void)
{
	CHECK_INT_EQ(test_windows_and_idle_intervals(), 0);
	CHECK_INT_EQ(test_fractional_average_uses_nanoseconds(), 0);
	CHECK_INT_EQ(test_unique_file_count_is_bounded(), 0);
	CHECK_INT_EQ(test_emit_cadence_reset_and_overflow(), 0);
	CHECK_INT_EQ(test_finalization_and_auxiliary_counters(), 0);
	CHECK_INT_EQ(test_absolute_runtime_samples_do_not_double_count(), 0);
	CHECK_INT_EQ(test_concurrent_updates_are_exact(), 0);
	return 0;
}
