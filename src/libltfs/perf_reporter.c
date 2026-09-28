/* SPDX-License-Identifier: BSD-3-Clause */

#include "perf_reporter.h"

#include <errno.h>
#include <string.h>

static void emit_snapshot(struct ltfs_perf_reporter *reporter)
{
	struct ltfs_perf_runtime_sample runtime = { 0 };
	struct ltfs_perf_snapshot snapshot;
	struct ltfs_event event;

	if (reporter->sample &&
		reporter->sample(reporter->sample_context, &runtime) == 0)
		(void)ltfs_perf_update_samples(reporter->counters,
			runtime.queue_fill_bytes, runtime.resident_bytes,
			runtime.retry_count, runtime.buffer_underrun_count);
	if (ltfs_perf_snapshot(reporter->counters, 1, &snapshot) < 0)
		return;
	memset(&event, 0, sizeof(event));
	event.phase = LTFS_PHASE_WRITING;
	event.status = LTFS_EVENT_PROGRESS;
	event.bytes_done = snapshot.total_bytes;
	event.files_done = snapshot.total_files;
	event.rate_bytes_per_second = snapshot.rate_valid ?
		snapshot.rate_bytes_per_second : 0;
	event.queue_fill_bytes = snapshot.queue_fill_bytes;
	event.buffer_underrun_count = snapshot.buffer_underrun_count;
	event.retry_count = snapshot.retry_count;
	event.memory_high_water_bytes = snapshot.memory_high_water_bytes;
	event.telemetry_overflowed = snapshot.overflowed;
	event.message_code = "telemetry.write.progress";
	(void)ltfs_event_emit(reporter->sink, &event);
}

static void *reporter_main(void *opaque)
{
	struct ltfs_perf_reporter *reporter = opaque;

	for (;;) {
		bool stop;
		pthread_mutex_lock(&reporter->mutex);
		while (!reporter->pending && !reporter->stopping)
			pthread_cond_wait(&reporter->condition, &reporter->mutex);
		stop = reporter->stopping;
		if (!reporter->pending && stop) {
			pthread_mutex_unlock(&reporter->mutex);
			break;
		}
		reporter->pending = false;
		pthread_mutex_unlock(&reporter->mutex);
		emit_snapshot(reporter);
		if (stop)
			break;
	}
	return NULL;
}

int ltfs_perf_reporter_init(struct ltfs_perf_reporter *reporter,
	struct ltfs_perf_counters *counters, struct ltfs_event_sink *sink,
	ltfs_perf_sample_fn sample, void *sample_context)
{
	int result;

	if (!reporter || !counters || !sink)
		return -EINVAL;
	memset(reporter, 0, sizeof(*reporter));
	result = pthread_mutex_init(&reporter->mutex, NULL);
	if (result != 0)
		return -result;
	result = pthread_cond_init(&reporter->condition, NULL);
	if (result != 0) {
		pthread_mutex_destroy(&reporter->mutex);
		return -result;
	}
	reporter->counters = counters;
	reporter->sink = sink;
	reporter->sample = sample;
	reporter->sample_context = sample_context;
	result = pthread_create(&reporter->thread, NULL, reporter_main, reporter);
	if (result != 0) {
		pthread_cond_destroy(&reporter->condition);
		pthread_mutex_destroy(&reporter->mutex);
		return -result;
	}
	reporter->initialized = true;
	reporter->running = true;
	return 0;
}

int ltfs_perf_reporter_notify(struct ltfs_perf_reporter *reporter)
{
	if (!reporter || !reporter->initialized)
		return -EINVAL;
	pthread_mutex_lock(&reporter->mutex);
	if (reporter->running) {
		reporter->pending = true;
		pthread_cond_signal(&reporter->condition);
	}
	pthread_mutex_unlock(&reporter->mutex);
	return 0;
}

void ltfs_perf_reporter_stop(struct ltfs_perf_reporter *reporter)
{
	if (!reporter || !reporter->initialized)
		return;
	pthread_mutex_lock(&reporter->mutex);
	if (!reporter->running) {
		pthread_mutex_unlock(&reporter->mutex);
		return;
	}
	reporter->running = false;
	reporter->stopping = true;
	pthread_cond_signal(&reporter->condition);
	pthread_mutex_unlock(&reporter->mutex);
	pthread_join(reporter->thread, NULL);
}

void ltfs_perf_reporter_destroy(struct ltfs_perf_reporter *reporter)
{
	if (!reporter || !reporter->initialized)
		return;
	ltfs_perf_reporter_stop(reporter);
	reporter->initialized = false;
	pthread_cond_destroy(&reporter->condition);
	pthread_mutex_destroy(&reporter->mutex);
}
