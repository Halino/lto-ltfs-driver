/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTFS_EVENTS_H
#define LTFS_EVENTS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define LTFS_EVENT_SCHEMA_VERSION 1
#define LTFS_EVENT_OPERATION_ID_SIZE 37
#define LTFS_EVENT_MAX_JSON_SIZE (16U * 1024U)

enum ltfs_event_phase {
	LTFS_PHASE_STARTING,
	LTFS_PHASE_MOUNTING,
	LTFS_PHASE_READY,
	LTFS_PHASE_WRITING,
	LTFS_PHASE_CLOSING_HANDLES,
	LTFS_PHASE_DRAINING_DATA,
	LTFS_PHASE_BUILDING_INDEX,
	LTFS_PHASE_WRITING_INDEX,
	LTFS_PHASE_UNMOUNTING,
	LTFS_PHASE_MEDIA_COMMITTED,
	LTFS_PHASE_PERSISTING_RECEIPT,
	LTFS_PHASE_UNLOADING,
	LTFS_PHASE_COMPLETE,
	LTFS_PHASE_FAILED,
};

enum ltfs_event_status {
	LTFS_EVENT_STARTED,
	LTFS_EVENT_PROGRESS,
	LTFS_EVENT_COMPLETE,
	LTFS_EVENT_FAILED,
};

struct ltfs_event {
	const char *volume_uuid;
	bool prior_generation_valid;
	uint64_t prior_generation;
	bool new_generation_valid;
	uint64_t new_generation;
	enum ltfs_event_phase phase;
	enum ltfs_event_status status;
	int result;
	bool device_close_result_valid;
	int device_close_result;
	uint64_t bytes_done;
	bool bytes_total_valid;
	uint64_t bytes_total;
	uint64_t files_done;
	bool files_total_valid;
	uint64_t files_total;
	uint64_t rate_bytes_per_second;
	uint64_t queue_fill_bytes;
	uint64_t buffer_underrun_count;
	uint64_t retry_count;
	uint64_t memory_high_water_bytes;
	bool telemetry_overflowed;
	uint64_t phase_elapsed_ns;
	bool index_progress_valid;
	uint64_t index_done;
	uint64_t index_total;
	bool eta_seconds_valid;
	uint64_t eta_seconds;
	const char *device_serial;
	const char *message_code;
};

struct ltfs_event_sink {
	pthread_mutex_t mutex;
	uint64_t seq;
	int fd;
	bool initialized;
	char operation_id[LTFS_EVENT_OPERATION_ID_SIZE];
	char json_buffer[LTFS_EVENT_MAX_JSON_SIZE];
};

/*
 * fd remains caller-owned. Initialization duplicates it with FD_CLOEXEC and
 * enables O_NONBLOCK on the shared open-file-description, so the caller's fd
 * and every other duplicate observe O_NONBLOCK too. The flag remains set after
 * close. The caller must not change file-status flags after successful init,
 * and may restore them only after all event producers quiesce.
 */
int ltfs_events_init(struct ltfs_event_sink *sink, int fd,
	const char *operation_id);
int ltfs_event_emit(struct ltfs_event_sink *sink,
	const struct ltfs_event *event);

/*
 * Emit and close require a sink returned by successful init. Close may run
 * concurrently with emit. It closes only the sink's duplicate; later emits
 * safely become no-ops. The caller must retain sink storage until all calling
 * threads have returned, and must not reinitialize a closed sink.
 */
void ltfs_events_close(struct ltfs_event_sink *sink);

#endif /* LTFS_EVENTS_H */
