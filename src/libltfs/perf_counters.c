/* SPDX-License-Identifier: LGPL-2.1-only */
/* Downstream modifications: 2026-08 through 2026-09; see LGPL-NOTICE. */

#include "perf_counters.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#define NS_PER_SECOND 1000000000ULL

static uint64_t system_monotonic_ns(void *context)
{
	struct timespec value;
	(void)context;
	if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
		return UINT64_MAX;
	return (uint64_t)value.tv_sec * NS_PER_SECOND + (uint64_t)value.tv_nsec;
}

static bool add_saturating(uint64_t *value, uint64_t increment)
{
	if (UINT64_MAX - *value < increment) {
		*value = UINT64_MAX;
		return false;
	}
	*value += increment;
	return true;
}

static int observed_now(struct ltfs_perf_counters *counters, uint64_t *now)
{
	*now = counters->config.monotonic_ns(counters->config.clock_context);
	if (*now == UINT64_MAX || *now < counters->last_observed_ns)
		return -ERANGE;
	counters->last_observed_ns = *now;
	return 0;
}

static void reset_locked(struct ltfs_perf_counters *counters, uint64_t now)
{
	memset(counters->buckets, 0, sizeof(counters->buckets));
	memset(counters->file_identities, 0, sizeof(counters->file_identities));
	memset(counters->phase_duration_ns, 0,
		sizeof(counters->phase_duration_ns));
	counters->started_ns = now;
	counters->last_observed_ns = now;
	counters->last_emit_second = 0;
	counters->total_bytes = 0;
	counters->total_files = 0;
	counters->queue_fill_bytes = 0;
	counters->buffer_underrun_count = 0;
	counters->retry_count = 0;
	counters->memory_high_water_bytes = 0;
	counters->emit_second_valid = false;
	counters->finalizing = false;
	counters->overflowed = false;
	counters->file_identity_overflowed = false;
}

int ltfs_perf_record_file(struct ltfs_perf_counters *counters,
	uint64_t identity)
{
	size_t start;
	size_t offset;
	int result = 0;
	if (!counters || !counters->initialized)
		return -EINVAL;
	start = (size_t)((identity ^ (identity >> 32)) %
		LTFS_PERF_FILE_ID_CAPACITY);
	pthread_mutex_lock(&counters->mutex);
	for (offset = 0; offset < LTFS_PERF_FILE_ID_CAPACITY; ++offset) {
		struct ltfs_perf_file_identity *entry =
			&counters->file_identities[(start + offset) %
				LTFS_PERF_FILE_ID_CAPACITY];
		if (entry->occupied && entry->value == identity)
			goto out;
		if (!entry->occupied) {
			entry->occupied = true;
			entry->value = identity;
			if (!add_saturating(&counters->total_files, 1)) {
				counters->overflowed = true;
				result = -EOVERFLOW;
			}
			goto out;
		}
	}
	counters->overflowed = true;
	counters->file_identity_overflowed = true;
	result = -ENOSPC;
out:
	pthread_mutex_unlock(&counters->mutex);
	return result;
}

int ltfs_perf_init(struct ltfs_perf_counters *counters,
	const struct ltfs_perf_config *config)
{
	uint64_t now;
	if (!counters || !config || config->memory_ceiling_bytes == 0 ||
		config->memory_ceiling_bytes > LTFS_PERF_MAX_MEMORY_CEILING)
		return -EINVAL;
	memset(counters, 0, sizeof(*counters));
	counters->config = *config;
	if (!counters->config.monotonic_ns)
		counters->config.monotonic_ns = system_monotonic_ns;
	now = counters->config.monotonic_ns(counters->config.clock_context);
	if (now == UINT64_MAX)
		return -EIO;
	if (pthread_mutex_init(&counters->mutex, NULL) != 0)
		return -EIO;
	reset_locked(counters, now);
	counters->initialized = true;
	return 0;
}

void ltfs_perf_destroy(struct ltfs_perf_counters *counters)
{
	if (!counters || !counters->initialized)
		return;
	pthread_mutex_destroy(&counters->mutex);
	counters->initialized = false;
}

int ltfs_perf_reset(struct ltfs_perf_counters *counters)
{
	uint64_t now;
	int result;
	if (!counters || !counters->initialized)
		return -EINVAL;
	pthread_mutex_lock(&counters->mutex);
	result = observed_now(counters, &now);
	if (!result)
		reset_locked(counters, now);
	pthread_mutex_unlock(&counters->mutex);
	return result;
}

int ltfs_perf_record_write(struct ltfs_perf_counters *counters,
	uint64_t bytes, uint64_t queue_fill_bytes,
	uint64_t resident_bytes, bool *emit_due)
{
	struct ltfs_perf_bucket *bucket;
	uint64_t now;
	uint64_t second;
	bool exact = true;
	int result;
	if (!counters || !counters->initialized || !emit_due)
		return -EINVAL;
	*emit_due = false;
	pthread_mutex_lock(&counters->mutex);
	result = observed_now(counters, &now);
	if (result)
		goto out;
	if (queue_fill_bytes > counters->config.memory_ceiling_bytes ||
		resident_bytes > counters->config.memory_ceiling_bytes) {
		result = -E2BIG;
		goto out;
	}
	second = now / NS_PER_SECOND;
	bucket = &counters->buckets[second % LTFS_PERF_BUCKET_COUNT];
	if (!bucket->valid || bucket->second != second) {
		bucket->valid = true;
		bucket->second = second;
		bucket->bytes = 0;
	}
	exact &= add_saturating(&bucket->bytes, bytes);
	exact &= add_saturating(&counters->total_bytes, bytes);
	counters->queue_fill_bytes = queue_fill_bytes;
	if (resident_bytes > counters->memory_high_water_bytes)
		counters->memory_high_water_bytes = resident_bytes;
	if (!counters->emit_second_valid ||
		counters->last_emit_second != second) {
		counters->emit_second_valid = true;
		counters->last_emit_second = second;
		*emit_due = true;
	}
	if (!exact) {
		counters->overflowed = true;
		result = -EOVERFLOW;
	}
out:
	pthread_mutex_unlock(&counters->mutex);
	return result;
}

static int record_unit(struct ltfs_perf_counters *counters, uint64_t *value)
{
	int result = 0;
	if (!counters || !counters->initialized)
		return -EINVAL;
	pthread_mutex_lock(&counters->mutex);
	if (!add_saturating(value, 1)) {
		counters->overflowed = true;
		result = -EOVERFLOW;
	}
	pthread_mutex_unlock(&counters->mutex);
	return result;
}

int ltfs_perf_record_retry(struct ltfs_perf_counters *counters)
{
	return counters ? record_unit(counters, &counters->retry_count) : -EINVAL;
}

int ltfs_perf_record_buffer_underrun(struct ltfs_perf_counters *counters)
{
	return counters ? record_unit(counters,
		&counters->buffer_underrun_count) : -EINVAL;
}

int ltfs_perf_update_samples(struct ltfs_perf_counters *counters,
	uint64_t queue_fill_bytes, uint64_t resident_bytes,
	uint64_t retry_count, uint64_t buffer_underrun_count)
{
	int result;
	if (!counters || !counters->initialized)
		return -EINVAL;
	pthread_mutex_lock(&counters->mutex);
	if (queue_fill_bytes > counters->config.memory_ceiling_bytes ||
		resident_bytes > counters->config.memory_ceiling_bytes) {
		queue_fill_bytes = queue_fill_bytes > counters->config.memory_ceiling_bytes ?
			counters->config.memory_ceiling_bytes : queue_fill_bytes;
		resident_bytes = resident_bytes > counters->config.memory_ceiling_bytes ?
			counters->config.memory_ceiling_bytes : resident_bytes;
		counters->overflowed = true;
		result = -E2BIG;
	} else
		result = 0;
	counters->queue_fill_bytes = queue_fill_bytes;
	if (resident_bytes > counters->memory_high_water_bytes)
		counters->memory_high_water_bytes = resident_bytes;
	counters->retry_count = retry_count;
	counters->buffer_underrun_count = buffer_underrun_count;
	pthread_mutex_unlock(&counters->mutex);
	return result;
}

int ltfs_perf_record_phase_duration(struct ltfs_perf_counters *counters,
	enum ltfs_perf_phase phase, uint64_t duration_ns)
{
	int result = 0;
	if (!counters || !counters->initialized || phase < 0 ||
		phase >= LTFS_PERF_PHASE_COUNT)
		return -EINVAL;
	pthread_mutex_lock(&counters->mutex);
	if (!add_saturating(&counters->phase_duration_ns[phase], duration_ns)) {
		counters->overflowed = true;
		result = -EOVERFLOW;
	}
	pthread_mutex_unlock(&counters->mutex);
	return result;
}

int ltfs_perf_set_finalizing(struct ltfs_perf_counters *counters,
	bool finalizing)
{
	if (!counters || !counters->initialized)
		return -EINVAL;
	pthread_mutex_lock(&counters->mutex);
	counters->finalizing = finalizing;
	pthread_mutex_unlock(&counters->mutex);
	return 0;
}

int ltfs_perf_snapshot(struct ltfs_perf_counters *counters,
	unsigned int window_seconds, struct ltfs_perf_snapshot *snapshot)
{
	uint64_t now;
	uint64_t now_second;
	uint64_t elapsed_seconds;
	uint64_t effective_window;
	uint64_t first_second;
	uint64_t bytes = 0;
	size_t index;
	int result;
	if (!counters || !counters->initialized || !snapshot ||
		(window_seconds != 1 && window_seconds != 10 && window_seconds != 60))
		return -EINVAL;
	pthread_mutex_lock(&counters->mutex);
	result = observed_now(counters, &now);
	if (result)
		goto out;
	memset(snapshot, 0, sizeof(*snapshot));
	snapshot->total_bytes = counters->total_bytes;
	snapshot->total_files = counters->total_files;
	snapshot->total_files_valid = !counters->file_identity_overflowed;
	snapshot->queue_fill_bytes = counters->queue_fill_bytes;
	snapshot->buffer_underrun_count = counters->buffer_underrun_count;
	snapshot->retry_count = counters->retry_count;
	snapshot->memory_high_water_bytes = counters->memory_high_water_bytes;
	memcpy(snapshot->phase_duration_ns, counters->phase_duration_ns,
		sizeof(snapshot->phase_duration_ns));
	snapshot->overflowed = counters->overflowed;
	snapshot->file_identity_overflowed = counters->file_identity_overflowed;
	if (counters->finalizing)
		goto out;
	elapsed_seconds = (now - counters->started_ns) / NS_PER_SECOND;
	if (elapsed_seconds == 0)
		goto out;
	now_second = now / NS_PER_SECOND;
	effective_window = elapsed_seconds < window_seconds ?
		elapsed_seconds : window_seconds;
	first_second = now_second - effective_window;
	for (index = 0; index < LTFS_PERF_BUCKET_COUNT; ++index) {
		const struct ltfs_perf_bucket *bucket = &counters->buckets[index];
		if (bucket->valid && bucket->second >= first_second &&
			bucket->second < now_second)
			add_saturating(&bytes, bucket->bytes);
	}
	snapshot->rate_valid = true;
	snapshot->rate_bytes_per_second = bytes / effective_window;
	snapshot->average_rate_valid = true;
	snapshot->average_bytes_per_second = (uint64_t)(
		((__uint128_t)counters->total_bytes * NS_PER_SECOND) /
		(now - counters->started_ns));
out:
	pthread_mutex_unlock(&counters->mutex);
	return result;
}
