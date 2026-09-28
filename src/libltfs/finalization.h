/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTFS_FINALIZATION_H
#define LTFS_FINALIZATION_H

#include <stdbool.h>
#include <stdint.h>

#define LTFS_FINALIZATION_ID_SIZE 37

#ifdef __cplusplus
extern "C" {
#endif

enum ltfs_finalization_phase {
	LTFS_FINALIZATION_NONE = 0,
	LTFS_FINALIZATION_CLOSING_HANDLES,
	LTFS_FINALIZATION_DRAINING_DATA,
	LTFS_FINALIZATION_BUILDING_INDEX,
	LTFS_FINALIZATION_WRITING_INDEX,
	LTFS_FINALIZATION_UNMOUNTING,
	LTFS_FINALIZATION_MEDIA_COMMITTED,
	LTFS_FINALIZATION_PERSISTING_RECEIPT,
	LTFS_FINALIZATION_UNLOADING,
	LTFS_FINALIZATION_COMPLETE,
	LTFS_FINALIZATION_FAILED,
	LTFS_FINALIZATION_PHASE_COUNT,
};

enum ltfs_finalization_event_status {
	LTFS_FINALIZATION_EVENT_STARTED,
	LTFS_FINALIZATION_EVENT_PROGRESS,
	LTFS_FINALIZATION_EVENT_COMPLETE,
	LTFS_FINALIZATION_EVENT_FAILED,
};

struct ltfs_finalization_event {
	char operation_id[LTFS_FINALIZATION_ID_SIZE];
	char volume_uuid[LTFS_FINALIZATION_ID_SIZE];
	uint64_t prior_generation;
	uint64_t new_generation;
	enum ltfs_finalization_phase phase;
	enum ltfs_finalization_event_status status;
	const char *message_code;
	int result;
	bool device_close_result_valid;
	int device_close_result;
	uint64_t phase_elapsed_ns;
	bool bytes_valid;
	uint64_t bytes;
	bool files_valid;
	uint64_t files;
	uint64_t rate_bytes_per_second;
	bool index_progress_valid;
	uint64_t index_done;
	uint64_t index_total;
};

struct ltfs_commit_receipt {
	char operation_id[LTFS_FINALIZATION_ID_SIZE];
	char volume_uuid[LTFS_FINALIZATION_ID_SIZE];
	uint64_t prior_generation;
	uint64_t new_generation;
	bool bytes_valid;
	uint64_t bytes;
	bool files_valid;
	uint64_t files;
	uint64_t phase_duration_ns[LTFS_FINALIZATION_PHASE_COUNT];
	uint64_t capture_duration_ns;
	uint64_t device_close_duration_ns;
	bool device_close_result_valid;
	int device_close_result;
	uint64_t catalog_ack_duration_ns;
	bool media_committed;
	bool catalog_acknowledged;
	bool cleanup_failed;
	int result;
};

struct ltfs_commit_ack {
	char operation_id[LTFS_FINALIZATION_ID_SIZE];
	char volume_uuid[LTFS_FINALIZATION_ID_SIZE];
	uint64_t generation;
	bool durable;
};

struct ltfs_finalization;

struct ltfs_finalization_ops {
	int (*drain_handles)(void *context);
	int (*flush_data)(void *context);
	int (*destroy_scheduler)(void *context);
	int (*destroy_kmi)(void *context);
	int (*commit_unmount)(void *context,
		struct ltfs_finalization *finalization);
	int (*capture_index)(void *context);
	int (*persist_receipt)(void *context,
		const struct ltfs_commit_receipt *receipt,
		struct ltfs_commit_ack *ack);
	int (*finalize_receipt)(void *context,
		const struct ltfs_commit_receipt *prepared_receipt,
		const struct ltfs_commit_receipt *terminal_receipt);
	int (*unload_media)(void *context);
	int (*close_device)(void *context);
	uint64_t (*monotonic_ns)(void *context);
	int (*emit)(void *context,
		const struct ltfs_finalization_event *event);
};

struct ltfs_finalization_config {
	void *context;
	const char *operation_id;
	const char *volume_uuid;
	uint64_t prior_generation;
	uint64_t new_generation;
	bool bytes_valid;
	uint64_t bytes;
	bool files_valid;
	uint64_t files;
	bool capture_enabled;
	bool unload_enabled;
	struct ltfs_finalization_ops ops;
};

struct ltfs_finalization {
	void *context;
	struct ltfs_finalization_ops ops;
	struct ltfs_commit_receipt receipt;
	struct ltfs_commit_receipt prepared_receipt;
	enum ltfs_finalization_phase phase;
	const char *phase_message_code;
	uint64_t phase_started_ns;
	bool phase_open;
	bool capture_enabled;
	bool unload_enabled;
	bool running;
	bool finished;
	bool scheduler_destroy_attempted;
	bool kmi_destroy_attempted;
	bool device_close_attempted;
	bool prepared_receipt_valid;
	int event_result;
};

int ltfs_finalization_init(struct ltfs_finalization *finalization,
	const struct ltfs_finalization_config *config);
int ltfs_finalization_advance(struct ltfs_finalization *finalization,
	enum ltfs_finalization_phase phase, const char *message_code);
int ltfs_finalization_index_progress(struct ltfs_finalization *finalization,
	uint64_t done, bool total_valid, uint64_t total);
void ltfs_finalization_set_receipt(struct ltfs_finalization *finalization,
	uint64_t new_generation, bool bytes_valid, uint64_t bytes,
	bool files_valid, uint64_t files,
	const char *volume_uuid);
void ltfs_finalization_set_media_identity(struct ltfs_finalization *finalization,
	uint64_t new_generation, const char *volume_uuid);
int ltfs_finalize(struct ltfs_finalization *finalization,
	struct ltfs_commit_receipt *receipt);

#ifdef __cplusplus
}
#endif

#endif /* LTFS_FINALIZATION_H */
