/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_PERF_REPORTER_H
#define LTO_LTFS_PERF_REPORTER_H

#include "ltfs_events.h"
#include "perf_counters.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct ltfs_perf_runtime_sample {
	uint64_t queue_fill_bytes;
	uint64_t resident_bytes;
	uint64_t retry_count;
	uint64_t buffer_underrun_count;
};

typedef int (*ltfs_perf_sample_fn)(void *context,
	struct ltfs_perf_runtime_sample *sample);

struct ltfs_perf_reporter {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	pthread_t thread;
	struct ltfs_perf_counters *counters;
	struct ltfs_event_sink *sink;
	ltfs_perf_sample_fn sample;
	void *sample_context;
	bool pending;
	bool stopping;
	bool running;
	bool initialized;
};

int ltfs_perf_reporter_init(struct ltfs_perf_reporter *reporter,
	struct ltfs_perf_counters *counters, struct ltfs_event_sink *sink,
	ltfs_perf_sample_fn sample, void *sample_context);
int ltfs_perf_reporter_notify(struct ltfs_perf_reporter *reporter);
void ltfs_perf_reporter_stop(struct ltfs_perf_reporter *reporter);
void ltfs_perf_reporter_destroy(struct ltfs_perf_reporter *reporter);

#endif /* LTO_LTFS_PERF_REPORTER_H */
