/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTFS_OPERATION_CONTROL_H
#define LTFS_OPERATION_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#define LTFS_OPERATION_ID_SIZE 37

enum ltfs_index_policy {
	LTFS_INDEX_POLICY_UNMOUNT,
	LTFS_INDEX_POLICY_INTERVAL,
	LTFS_INDEX_POLICY_LEGACY,
};

enum ltfs_operation_options_result {
	LTFS_OPERATION_OPTIONS_OK = 0,
	LTFS_OPERATION_OPTIONS_EVENT_FD,
	LTFS_OPERATION_OPTIONS_EVENT_REQUIRED,
	LTFS_OPERATION_OPTIONS_EVENT_SCHEMA,
	LTFS_OPERATION_OPTIONS_OPERATION_ID,
	LTFS_OPERATION_OPTIONS_INDEX_POLICY,
	LTFS_OPERATION_OPTIONS_INDEX_INTERVAL,
	LTFS_OPERATION_OPTIONS_INDEX_CONFLICT,
};

struct ltfs_operation_options {
	bool events_enabled;
	/* Caller-owned write end of a dedicated pipe; never reused for other I/O. */
	int event_fd;
	unsigned int event_schema;
	char operation_id[LTFS_OPERATION_ID_SIZE];
	enum ltfs_index_policy index_policy;
	uint64_t interval_bytes;
	unsigned int interval_seconds;
	bool index_policy_explicit;
	bool legacy_sync_present;
};

typedef void (*ltfs_operation_cancel_fn)(void *opaque);
typedef int (*ltfs_interval_step_fn)(void *opaque);
typedef int (*ltfs_interval_now_fn)(void *opaque, uint64_t *now_ns);

struct ltfs_operation_failure {
	int result;
	ltfs_operation_cancel_fn cancel;
	void *cancel_opaque;
};

struct ltfs_interval_control;

enum ltfs_operation_options_result ltfs_operation_options_parse(
	int argc, char *const argv[], struct ltfs_operation_options *options);
const char *ltfs_operation_options_message_code(
	enum ltfs_operation_options_result result);
int ltfs_operation_result_preserve(int operational_result,
	int cleanup_result);
void ltfs_operation_failure_init(struct ltfs_operation_failure *failure,
	ltfs_operation_cancel_fn cancel, void *cancel_opaque);
bool ltfs_operation_failure_record(struct ltfs_operation_failure *failure,
	int result);
int ltfs_operation_failure_result(
	const struct ltfs_operation_failure *failure);
int ltfs_operation_write_gate(
	const struct ltfs_operation_failure *failure);

int ltfs_interval_control_create(uint64_t byte_threshold,
	unsigned int seconds_threshold, uint64_t now_ns,
	struct ltfs_interval_control **control);
void ltfs_interval_control_add_bytes(struct ltfs_interval_control *control,
	uint64_t bytes);
bool ltfs_interval_control_claim_at(struct ltfs_interval_control *control,
	uint64_t now_ns);
bool ltfs_interval_control_wait_claim(struct ltfs_interval_control *control);
void ltfs_interval_control_complete_at(struct ltfs_interval_control *control,
	int result, uint64_t completed_at_ns);
int ltfs_interval_control_error(struct ltfs_interval_control *control);
int ltfs_interval_commit_execute(struct ltfs_interval_control *control,
	struct ltfs_operation_failure *failure, ltfs_interval_step_fn flush,
	ltfs_interval_step_fn write_index, ltfs_interval_now_fn now,
	void *opaque);
void ltfs_interval_control_stop(struct ltfs_interval_control *control);
void ltfs_interval_control_destroy(struct ltfs_interval_control *control);

#endif /* LTFS_OPERATION_CONTROL_H */
