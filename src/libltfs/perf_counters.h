/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_PERF_COUNTERS_H
#define LTO_LTFS_PERF_COUNTERS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define LTFS_PERF_BUCKET_COUNT 61
#define LTFS_PERF_FILE_ID_CAPACITY 4096
#define LTFS_PERF_MAX_MEMORY_CEILING (512ULL * 1024ULL * 1024ULL)

enum ltfs_perf_phase {
	LTFS_PERF_PHASE_CLOSING_HANDLES = 0,
	LTFS_PERF_PHASE_DRAINING_DATA,
	LTFS_PERF_PHASE_BUILDING_INDEX,
	LTFS_PERF_PHASE_WRITING_INDEX,
	LTFS_PERF_PHASE_UNMOUNT,
	LTFS_PERF_PHASE_UNLOAD,
	LTFS_PERF_PHASE_COUNT,
};

struct ltfs_perf_bucket {
	uint64_t second;
	uint64_t bytes;
	bool valid;
};

struct ltfs_perf_file_identity {
	uint64_t value;
	bool occupied;
};

struct ltfs_perf_config {
	uint64_t (*monotonic_ns)(void *context);
	void *clock_context;
	uint64_t memory_ceiling_bytes;
};

struct ltfs_perf_snapshot {
	bool rate_valid;
	uint64_t rate_bytes_per_second;
	bool average_rate_valid;
	uint64_t average_bytes_per_second;
	uint64_t total_bytes;
	uint64_t total_files;
	bool total_files_valid;
	uint64_t queue_fill_bytes;
	uint64_t buffer_underrun_count;
	uint64_t retry_count;
	uint64_t memory_high_water_bytes;
	uint64_t phase_duration_ns[LTFS_PERF_PHASE_COUNT];
	bool overflowed;
	bool file_identity_overflowed;
};

struct ltfs_perf_counters {
	pthread_mutex_t mutex;
	struct ltfs_perf_config config;
	struct ltfs_perf_bucket buckets[LTFS_PERF_BUCKET_COUNT];
	struct ltfs_perf_file_identity file_identities[LTFS_PERF_FILE_ID_CAPACITY];
	uint64_t started_ns;
	uint64_t last_observed_ns;
	uint64_t last_emit_second;
	uint64_t total_bytes;
	uint64_t total_files;
	uint64_t queue_fill_bytes;
	uint64_t buffer_underrun_count;
	uint64_t retry_count;
	uint64_t memory_high_water_bytes;
	uint64_t phase_duration_ns[LTFS_PERF_PHASE_COUNT];
	bool emit_second_valid;
	bool finalizing;
	bool overflowed;
	bool file_identity_overflowed;
	bool initialized;
};

int ltfs_perf_init(struct ltfs_perf_counters *counters,
	const struct ltfs_perf_config *config);
void ltfs_perf_destroy(struct ltfs_perf_counters *counters);
int ltfs_perf_reset(struct ltfs_perf_counters *counters);
int ltfs_perf_record_write(struct ltfs_perf_counters *counters,
	uint64_t bytes, uint64_t queue_fill_bytes,
	uint64_t resident_bytes, bool *emit_due);
int ltfs_perf_record_file(struct ltfs_perf_counters *counters,
	uint64_t identity);
int ltfs_perf_record_retry(struct ltfs_perf_counters *counters);
int ltfs_perf_record_buffer_underrun(struct ltfs_perf_counters *counters);
int ltfs_perf_update_samples(struct ltfs_perf_counters *counters,
	uint64_t queue_fill_bytes, uint64_t resident_bytes,
	uint64_t retry_count, uint64_t buffer_underrun_count);
int ltfs_perf_record_phase_duration(struct ltfs_perf_counters *counters,
	enum ltfs_perf_phase phase, uint64_t duration_ns);
int ltfs_perf_set_finalizing(struct ltfs_perf_counters *counters,
	bool finalizing);
int ltfs_perf_snapshot(struct ltfs_perf_counters *counters,
	unsigned int window_seconds, struct ltfs_perf_snapshot *snapshot);

#endif /* LTO_LTFS_PERF_COUNTERS_H */
