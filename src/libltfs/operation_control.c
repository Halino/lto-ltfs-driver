/* SPDX-License-Identifier: BSD-3-Clause */

#include "operation_control.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_SECOND UINT64_C(1000000000)

struct ltfs_interval_control {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	uint64_t byte_threshold;
	uint64_t interval_ns;
	uint64_t dirty_bytes;
	uint64_t deadline_ns;
	bool commit_active;
	bool stopped;
	int terminal_error;
};

static bool parse_uint64(const char *value, uint64_t maximum, uint64_t *parsed)
{
	uint64_t result = 0;
	const unsigned char *cursor = (const unsigned char *)value;

	if (!value || !*value)
		return false;
	while (*cursor) {
		unsigned int digit;
		if (*cursor < '0' || *cursor > '9')
			return false;
		digit = *cursor - '0';
		if (result > (maximum - digit) / 10)
			return false;
		result = result * 10 + digit;
		++cursor;
	}
	*parsed = result;
	return true;
}

static bool valid_uuid(const char *value)
{
	static const size_t hyphens[] = { 8, 13, 18, 23 };
	size_t i;
	size_t next_hyphen = 0;

	if (!value || strlen(value) != LTFS_OPERATION_ID_SIZE - 1)
		return false;
	for (i = 0; i < LTFS_OPERATION_ID_SIZE - 1; ++i) {
		if (next_hyphen < sizeof(hyphens) / sizeof(hyphens[0]) &&
		    i == hyphens[next_hyphen]) {
			if (value[i] != '-')
				return false;
			++next_hyphen;
		} else if (!((value[i] >= '0' && value[i] <= '9') ||
		             (value[i] >= 'a' && value[i] <= 'f') ||
		             (value[i] >= 'A' && value[i] <= 'F')))
			return false;
	}
	if (value[14] < '1' || value[14] > '5')
		return false;
	return value[19] == '8' || value[19] == '9' ||
		value[19] == 'a' || value[19] == 'A' ||
		value[19] == 'b' || value[19] == 'B';
}

static bool contains_sync_type(const char *value)
{
	const char *cursor = value;

	while (cursor && *cursor) {
		if (!strncmp(cursor, "sync_type=", 10))
			return true;
		cursor = strchr(cursor, ',');
		if (cursor)
			++cursor;
	}
	return false;
}

static bool valid_event_fd(int fd)
{
	struct stat status;
	int flags;

	if (fd <= STDERR_FILENO)
		return false;
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || (flags & O_ACCMODE) != O_WRONLY)
		return false;
	if (fstat(fd, &status) < 0)
		return false;
	return S_ISFIFO(status.st_mode);
}

enum ltfs_operation_options_result ltfs_operation_options_parse(
	int argc, char *const argv[], struct ltfs_operation_options *options)
{
	bool event_fd_set = false;
	bool event_schema_set = false;
	bool operation_id_set = false;
	bool interval_bytes_set = false;
	bool interval_seconds_set = false;
	uint64_t value;
	int i;

	if (!options || argc < 0 || (argc && !argv))
		return LTFS_OPERATION_OPTIONS_EVENT_REQUIRED;
	memset(options, 0, sizeof(*options));
	options->event_fd = -1;
	options->index_policy = LTFS_INDEX_POLICY_UNMOUNT;

	for (i = 1; i < argc; ++i) {
		const char *argument = argv[i];
		const char *option_value;
		if (!argument)
			continue;
		if (!strncmp(argument, "--event-fd=", 11)) {
			if (event_fd_set || !parse_uint64(argument + 11, INT_MAX, &value))
				return LTFS_OPERATION_OPTIONS_EVENT_FD;
			options->event_fd = (int)value;
			event_fd_set = true;
		} else if (!strcmp(argument, "--event-fd") ||
		           !strncmp(argument, "--event-fd", 10))
			return LTFS_OPERATION_OPTIONS_EVENT_FD;
		else if (!strncmp(argument, "--event-schema=", 15)) {
			if (event_schema_set || strcmp(argument + 15, "1"))
				return LTFS_OPERATION_OPTIONS_EVENT_SCHEMA;
			options->event_schema = 1;
			event_schema_set = true;
		} else if (!strcmp(argument, "--event-schema") ||
		           !strncmp(argument, "--event-schema", 14))
			return LTFS_OPERATION_OPTIONS_EVENT_SCHEMA;
		else if (!strncmp(argument, "--operation-id=", 15)) {
			if (operation_id_set || !valid_uuid(argument + 15))
				return LTFS_OPERATION_OPTIONS_OPERATION_ID;
			memcpy(options->operation_id, argument + 15,
				LTFS_OPERATION_ID_SIZE);
			operation_id_set = true;
		} else if (!strcmp(argument, "--operation-id") ||
		           !strncmp(argument, "--operation-id", 14))
			return LTFS_OPERATION_OPTIONS_OPERATION_ID;
		else if (!strncmp(argument, "--index-policy=", 15)) {
			if (options->index_policy_explicit)
				return LTFS_OPERATION_OPTIONS_INDEX_POLICY;
			option_value = argument + 15;
			if (!strcmp(option_value, "unmount"))
				options->index_policy = LTFS_INDEX_POLICY_UNMOUNT;
			else if (!strcmp(option_value, "interval"))
				options->index_policy = LTFS_INDEX_POLICY_INTERVAL;
			else
				return LTFS_OPERATION_OPTIONS_INDEX_POLICY;
			options->index_policy_explicit = true;
		} else if (!strcmp(argument, "--index-policy") ||
		           !strncmp(argument, "--index-policy", 14))
			return LTFS_OPERATION_OPTIONS_INDEX_POLICY;
		else if (!strncmp(argument, "--index-interval-bytes=", 23)) {
			if (interval_bytes_set ||
			    !parse_uint64(argument + 23, UINT64_MAX, &value) || value == 0)
				return LTFS_OPERATION_OPTIONS_INDEX_INTERVAL;
			options->interval_bytes = value;
			interval_bytes_set = true;
		} else if (!strcmp(argument, "--index-interval-bytes") ||
		           !strncmp(argument, "--index-interval-bytes", 22))
			return LTFS_OPERATION_OPTIONS_INDEX_INTERVAL;
		else if (!strncmp(argument, "--index-interval-seconds=", 25)) {
			if (interval_seconds_set ||
			    !parse_uint64(argument + 25, UINT_MAX, &value) || value == 0)
				return LTFS_OPERATION_OPTIONS_INDEX_INTERVAL;
			options->interval_seconds = (unsigned int)value;
			interval_seconds_set = true;
		} else if (!strcmp(argument, "--index-interval-seconds") ||
		           !strncmp(argument, "--index-interval-seconds", 24))
			return LTFS_OPERATION_OPTIONS_INDEX_INTERVAL;
		else if (!strncmp(argument, "-osync_type=", 12))
			options->legacy_sync_present = true;
		else if (!strcmp(argument, "-o") && i + 1 < argc && argv[i + 1] &&
		         contains_sync_type(argv[i + 1]))
			options->legacy_sync_present = true;
	}

	if (event_fd_set && !valid_event_fd(options->event_fd))
		return LTFS_OPERATION_OPTIONS_EVENT_FD;
	if (event_fd_set || event_schema_set || operation_id_set) {
		if (!event_fd_set || !event_schema_set || !operation_id_set)
			return LTFS_OPERATION_OPTIONS_EVENT_REQUIRED;
		options->events_enabled = true;
	}
	if (options->legacy_sync_present &&
	    (options->index_policy_explicit || interval_bytes_set || interval_seconds_set))
		return LTFS_OPERATION_OPTIONS_INDEX_CONFLICT;
	if (options->legacy_sync_present) {
		options->index_policy = LTFS_INDEX_POLICY_LEGACY;
		return LTFS_OPERATION_OPTIONS_OK;
	}
	if (options->index_policy == LTFS_INDEX_POLICY_INTERVAL) {
		if (!interval_bytes_set || !interval_seconds_set)
			return LTFS_OPERATION_OPTIONS_INDEX_INTERVAL;
	} else if (interval_bytes_set || interval_seconds_set)
		return LTFS_OPERATION_OPTIONS_INDEX_CONFLICT;
	return LTFS_OPERATION_OPTIONS_OK;
}

const char *ltfs_operation_options_message_code(
	enum ltfs_operation_options_result result)
{
	switch (result) {
	case LTFS_OPERATION_OPTIONS_OK: return "options.valid";
	case LTFS_OPERATION_OPTIONS_EVENT_FD: return "options.event_fd.invalid";
	case LTFS_OPERATION_OPTIONS_EVENT_REQUIRED: return "options.event.required";
	case LTFS_OPERATION_OPTIONS_EVENT_SCHEMA: return "options.event_schema.unsupported";
	case LTFS_OPERATION_OPTIONS_OPERATION_ID: return "options.operation_id.invalid";
	case LTFS_OPERATION_OPTIONS_INDEX_POLICY: return "options.index_policy.invalid";
	case LTFS_OPERATION_OPTIONS_INDEX_INTERVAL: return "options.index_interval.invalid";
	case LTFS_OPERATION_OPTIONS_INDEX_CONFLICT: return "options.index_policy.conflict";
	}
	return "options.invalid";
}

int ltfs_operation_result_preserve(int operational_result,
	int cleanup_result)
{
	return operational_result != 0 ? operational_result : cleanup_result;
}

void ltfs_operation_failure_init(struct ltfs_operation_failure *failure,
	ltfs_operation_cancel_fn cancel, void *cancel_opaque)
{
	if (!failure)
		return;
	__atomic_store_n(&failure->result, 0, __ATOMIC_RELEASE);
	failure->cancel = cancel;
	failure->cancel_opaque = cancel_opaque;
}

bool ltfs_operation_failure_record(struct ltfs_operation_failure *failure,
	int result)
{
	int expected = 0;

	if (!failure || result == 0)
		return false;
	if (!__atomic_compare_exchange_n(&failure->result, &expected, result,
		false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return false;
	if (failure->cancel)
		failure->cancel(failure->cancel_opaque);
	return true;
}

int ltfs_operation_failure_result(
	const struct ltfs_operation_failure *failure)
{
	if (!failure)
		return 0;
	return __atomic_load_n(&failure->result, __ATOMIC_ACQUIRE);
}

int ltfs_operation_write_gate(
	const struct ltfs_operation_failure *failure)
{
	return ltfs_operation_failure_result(failure);
}

static uint64_t saturating_add(uint64_t left, uint64_t right)
{
	return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

static uint64_t monotonic_now_ns(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * NS_PER_SECOND + (uint64_t)now.tv_nsec;
}

static bool claim_locked(struct ltfs_interval_control *control, uint64_t now_ns)
{
	bool byte_due = control->byte_threshold > 0 &&
		control->dirty_bytes >= control->byte_threshold;
	bool time_due = now_ns >= control->deadline_ns;

	if (control->stopped || control->commit_active || (!byte_due && !time_due))
		return false;
	control->commit_active = true;
	control->dirty_bytes = 0;
	return true;
}

int ltfs_interval_control_create(uint64_t byte_threshold,
	unsigned int seconds_threshold, uint64_t now_ns,
	struct ltfs_interval_control **control)
{
	struct ltfs_interval_control *created;
	pthread_condattr_t attributes;
	int ret;

	if (!control || seconds_threshold == 0)
		return -EINVAL;
	*control = NULL;
	created = calloc(1, sizeof(*created));
	if (!created)
		return -ENOMEM;
	ret = pthread_mutex_init(&created->mutex, NULL);
	if (ret != 0) {
		free(created);
		return -ret;
	}
	ret = pthread_condattr_init(&attributes);
	if (ret != 0) {
		pthread_mutex_destroy(&created->mutex);
		free(created);
		return -ret;
	}
	ret = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
	if (ret == 0)
		ret = pthread_cond_init(&created->condition, &attributes);
	pthread_condattr_destroy(&attributes);
	if (ret != 0) {
		pthread_mutex_destroy(&created->mutex);
		free(created);
		return -ret;
	}
	created->byte_threshold = byte_threshold;
	created->interval_ns = (uint64_t)seconds_threshold * NS_PER_SECOND;
	created->deadline_ns = saturating_add(now_ns, created->interval_ns);
	*control = created;
	return 0;
}

void ltfs_interval_control_add_bytes(struct ltfs_interval_control *control,
	uint64_t bytes)
{
	if (!control || bytes == 0)
		return;
	pthread_mutex_lock(&control->mutex);
	if (!control->stopped) {
		control->dirty_bytes = saturating_add(control->dirty_bytes, bytes);
		if (control->byte_threshold > 0 &&
		    control->dirty_bytes >= control->byte_threshold)
			pthread_cond_signal(&control->condition);
	}
	pthread_mutex_unlock(&control->mutex);
}

bool ltfs_interval_control_claim_at(struct ltfs_interval_control *control,
	uint64_t now_ns)
{
	bool claimed;
	if (!control)
		return false;
	pthread_mutex_lock(&control->mutex);
	claimed = claim_locked(control, now_ns);
	pthread_mutex_unlock(&control->mutex);
	return claimed;
}

bool ltfs_interval_control_wait_claim(struct ltfs_interval_control *control)
{
	struct timespec deadline;
	uint64_t now_ns;
	int ret;
	if (!control)
		return false;
	pthread_mutex_lock(&control->mutex);
	while (!control->stopped) {
		now_ns = monotonic_now_ns();
		if (claim_locked(control, now_ns)) {
			pthread_mutex_unlock(&control->mutex);
			return true;
		}
		deadline.tv_sec = (time_t)(control->deadline_ns / NS_PER_SECOND);
		deadline.tv_nsec = (long)(control->deadline_ns % NS_PER_SECOND);
		ret = pthread_cond_timedwait(&control->condition, &control->mutex,
			&deadline);
		if (ret != 0 && ret != ETIMEDOUT) {
			control->terminal_error = -ret;
			control->stopped = true;
			pthread_mutex_unlock(&control->mutex);
			return false;
		}
	}
	pthread_mutex_unlock(&control->mutex);
	return false;
}

void ltfs_interval_control_complete_at(struct ltfs_interval_control *control,
	int result, uint64_t completed_at_ns)
{
	if (!control)
		return;
	pthread_mutex_lock(&control->mutex);
	control->commit_active = false;
	if (result != 0) {
		if (control->terminal_error == 0)
			control->terminal_error = result;
		control->stopped = true;
	} else
		control->deadline_ns = saturating_add(completed_at_ns,
			control->interval_ns);
	pthread_cond_broadcast(&control->condition);
	pthread_mutex_unlock(&control->mutex);
}

int ltfs_interval_control_error(struct ltfs_interval_control *control)
{
	int result;
	if (!control)
		return 0;
	pthread_mutex_lock(&control->mutex);
	result = control->terminal_error;
	pthread_mutex_unlock(&control->mutex);
	return result;
}

int ltfs_interval_commit_execute(struct ltfs_interval_control *control,
	struct ltfs_operation_failure *failure, ltfs_interval_step_fn flush,
	ltfs_interval_step_fn write_index, ltfs_interval_now_fn now,
	void *opaque)
{
	uint64_t completed_at_ns = 0;
	int result;

	if (!control || !flush || !write_index || !now)
		return -EINVAL;
	result = flush(opaque);
	if (result == 0)
		result = write_index(opaque);
	if (result == 0)
		result = now(opaque, &completed_at_ns);
	if (result > 0)
		result = -result;
	ltfs_interval_control_complete_at(control, result, completed_at_ns);
	if (result != 0)
		ltfs_operation_failure_record(failure, result);
	return result;
}

void ltfs_interval_control_stop(struct ltfs_interval_control *control)
{
	if (!control)
		return;
	pthread_mutex_lock(&control->mutex);
	control->stopped = true;
	pthread_cond_broadcast(&control->condition);
	pthread_mutex_unlock(&control->mutex);
}

void ltfs_interval_control_destroy(struct ltfs_interval_control *control)
{
	if (!control)
		return;
	pthread_cond_destroy(&control->condition);
	pthread_mutex_destroy(&control->mutex);
	free(control);
}
