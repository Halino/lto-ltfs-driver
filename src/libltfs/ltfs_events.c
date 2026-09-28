/* SPDX-License-Identifier: BSD-3-Clause */

#include "ltfs_events.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LTFS_EVENT_MAX_MESSAGE_CODE 128U
#define LTFS_EVENT_MAX_DEVICE_SERIAL 256U

#ifdef LTFS_EVENTS_WRITE
extern ssize_t LTFS_EVENTS_WRITE(int fd, const void *buffer, size_t count);
#else
#define LTFS_EVENTS_WRITE write
#endif

struct json_buffer {
	char *data;
	size_t length;
	size_t capacity;
};

static bool valid_uuid(const char *value)
{
	size_t i;

	if (!value || strlen(value) != LTFS_EVENT_OPERATION_ID_SIZE - 1)
		return false;
	for (i = 0; i < LTFS_EVENT_OPERATION_ID_SIZE - 1; ++i) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (value[i] != '-')
				return false;
		} else if (!isxdigit((unsigned char)value[i]))
			return false;
	}
	if (value[14] < '1' || value[14] > '5')
		return false;
	return value[19] == '8' || value[19] == '9' ||
		value[19] == 'a' || value[19] == 'b' ||
		value[19] == 'A' || value[19] == 'B';
}

static bool valid_message_code(const char *value)
{
	size_t i;

	if (!value || !value[0])
		return false;
	for (i = 0; i <= LTFS_EVENT_MAX_MESSAGE_CODE && value[i]; ++i) {
		unsigned char byte = (unsigned char)value[i];
		if (!(byte >= 'a' && byte <= 'z') &&
		    !(byte >= '0' && byte <= '9') &&
		    byte != '.' && byte != '_' && byte != '-')
			return false;
	}
	return i <= LTFS_EVENT_MAX_MESSAGE_CODE;
}

static bool valid_utf8(const char *value, size_t maximum)
{
	const unsigned char *bytes = (const unsigned char *)value;
	size_t i = 0;

	if (!value)
		return true;
	while (bytes[i]) {
		unsigned char byte = bytes[i];
		size_t remaining;

		if (i >= maximum)
			return false;
		if (byte < 0x80) {
			++i;
			continue;
		}
		if (byte >= 0xC2 && byte <= 0xDF)
			remaining = 1;
		else if (byte >= 0xE0 && byte <= 0xEF)
			remaining = 2;
		else if (byte >= 0xF0 && byte <= 0xF4)
			remaining = 3;
		else
			return false;
		if (i + remaining >= maximum)
			return false;
		if (remaining >= 1 && (bytes[i + 1] & 0xC0) != 0x80)
			return false;
		if (remaining >= 2 && (bytes[i + 2] & 0xC0) != 0x80)
			return false;
		if (remaining == 3 && (bytes[i + 3] & 0xC0) != 0x80)
			return false;
		if (byte == 0xE0 && bytes[i + 1] < 0xA0)
			return false;
		if (byte == 0xED && bytes[i + 1] >= 0xA0)
			return false;
		if (byte == 0xF0 && bytes[i + 1] < 0x90)
			return false;
		if (byte == 0xF4 && bytes[i + 1] >= 0x90)
			return false;
		i += remaining + 1;
	}
	return true;
}

static const char *phase_name(enum ltfs_event_phase phase)
{
	switch (phase) {
	case LTFS_PHASE_STARTING: return "STARTING";
	case LTFS_PHASE_MOUNTING: return "MOUNTING";
	case LTFS_PHASE_READY: return "READY";
	case LTFS_PHASE_WRITING: return "WRITING";
	case LTFS_PHASE_CLOSING_HANDLES: return "CLOSING_HANDLES";
	case LTFS_PHASE_DRAINING_DATA: return "DRAINING_DATA";
	case LTFS_PHASE_BUILDING_INDEX: return "BUILDING_INDEX";
	case LTFS_PHASE_WRITING_INDEX: return "WRITING_INDEX";
	case LTFS_PHASE_UNMOUNTING: return "UNMOUNTING";
	case LTFS_PHASE_MEDIA_COMMITTED: return "MEDIA_COMMITTED";
	case LTFS_PHASE_PERSISTING_RECEIPT: return "PERSISTING_RECEIPT";
	case LTFS_PHASE_UNLOADING: return "UNLOADING";
	case LTFS_PHASE_COMPLETE: return "COMPLETE";
	case LTFS_PHASE_FAILED: return "FAILED";
	}
	return NULL;
}

static const char *status_name(enum ltfs_event_status status)
{
	switch (status) {
	case LTFS_EVENT_STARTED: return "started";
	case LTFS_EVENT_PROGRESS: return "progress";
	case LTFS_EVENT_COMPLETE: return "complete";
	case LTFS_EVENT_FAILED: return "failed";
	}
	return NULL;
}

static int reserve_buffer(struct json_buffer *buffer, size_t additional)
{
	size_t required;

	if (additional > LTFS_EVENT_MAX_JSON_SIZE - buffer->length)
		return -E2BIG;
	required = buffer->length + additional;
	return required <= buffer->capacity ? 0 : -E2BIG;
}

static int append_bytes(struct json_buffer *buffer, const char *value,
	size_t length)
{
	int ret = reserve_buffer(buffer, length);

	if (ret < 0)
		return ret;
	memcpy(buffer->data + buffer->length, value, length);
	buffer->length += length;
	return 0;
}

static int append_text(struct json_buffer *buffer, const char *value)
{
	return append_bytes(buffer, value, strlen(value));
}

static int append_uint64(struct json_buffer *buffer, uint64_t value)
{
	char number[32];
	int length = snprintf(number, sizeof(number), "%" PRIu64, value);

	if (length < 0 || (size_t)length >= sizeof(number))
		return -EOVERFLOW;
	return append_bytes(buffer, number, (size_t)length);
}

static int append_int64(struct json_buffer *buffer, int64_t value)
{
	char number[32];
	int length = snprintf(number, sizeof(number), "%" PRId64, value);

	if (length < 0 || (size_t)length >= sizeof(number))
		return -EOVERFLOW;
	return append_bytes(buffer, number, (size_t)length);
}

static int append_json_string(struct json_buffer *buffer, const char *value)
{
	static const char hex[] = "0123456789abcdef";
	const unsigned char *bytes = (const unsigned char *)value;
	int ret;

	ret = append_text(buffer, "\"");
	if (ret < 0)
		return ret;
	for (; *bytes; ++bytes) {
		char escaped[6];
		const char *short_escape = NULL;

		switch (*bytes) {
		case '"': short_escape = "\\\""; break;
		case '\\': short_escape = "\\\\"; break;
		case '\b': short_escape = "\\b"; break;
		case '\f': short_escape = "\\f"; break;
		case '\n': short_escape = "\\n"; break;
		case '\r': short_escape = "\\r"; break;
		case '\t': short_escape = "\\t"; break;
		default: break;
		}
		if (short_escape)
			ret = append_text(buffer, short_escape);
		else if (*bytes < 0x20) {
			escaped[0] = '\\';
			escaped[1] = 'u';
			escaped[2] = '0';
			escaped[3] = '0';
			escaped[4] = hex[*bytes >> 4];
			escaped[5] = hex[*bytes & 0x0F];
			ret = append_bytes(buffer, escaped, sizeof(escaped));
		} else
			ret = append_bytes(buffer, (const char *)bytes, 1);
		if (ret < 0)
			return ret;
	}
	return append_text(buffer, "\"");
}

static int append_nullable_uint64(struct json_buffer *buffer, bool valid,
	uint64_t value)
{
	return valid ? append_uint64(buffer, value) : append_text(buffer, "null");
}

static int append_nullable_int64(struct json_buffer *buffer, bool valid,
	int64_t value)
{
	return valid ? append_int64(buffer, value) : append_text(buffer, "null");
}

static int append_event_json(struct json_buffer *buffer,
	const struct ltfs_event_sink *sink, const struct ltfs_event *event,
	uint64_t seq, uint64_t monotonic_ns, const char *wall_time)
{
	const char *phase = phase_name(event->phase);
	const char *status = status_name(event->status);
	int ret;

#define APPEND_TEXT(value) do { \
	ret = append_text(buffer, (value)); \
	if (ret < 0) return ret; \
} while (0)
#define APPEND_STRING(value) do { \
	ret = append_json_string(buffer, (value)); \
	if (ret < 0) return ret; \
} while (0)
#define APPEND_UINT(value) do { \
	ret = append_uint64(buffer, (value)); \
	if (ret < 0) return ret; \
} while (0)
	APPEND_TEXT("{\"schema\":");
	APPEND_UINT(LTFS_EVENT_SCHEMA_VERSION);
	APPEND_TEXT(",\"operation_id\":");
	APPEND_STRING(sink->operation_id);
	APPEND_TEXT(",\"volume_uuid\":");
	if (event->volume_uuid)
		APPEND_STRING(event->volume_uuid);
	else
		APPEND_TEXT("null");
	APPEND_TEXT(",\"prior_generation\":");
	ret = append_nullable_uint64(buffer, event->prior_generation_valid,
		event->prior_generation);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"new_generation\":");
	ret = append_nullable_uint64(buffer, event->new_generation_valid,
		event->new_generation);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"seq\":");
	APPEND_UINT(seq);
	APPEND_TEXT(",\"monotonic_ns\":");
	APPEND_UINT(monotonic_ns);
	APPEND_TEXT(",\"wall_time\":");
	APPEND_STRING(wall_time);
	APPEND_TEXT(",\"phase\":");
	APPEND_STRING(phase);
	APPEND_TEXT(",\"status\":");
	APPEND_STRING(status);
	APPEND_TEXT(",\"result\":");
	ret = append_int64(buffer, event->result);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"device_close_result\":");
	ret = append_nullable_int64(buffer, event->device_close_result_valid,
		event->device_close_result);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"bytes_done\":");
	APPEND_UINT(event->bytes_done);
	APPEND_TEXT(",\"bytes_total\":");
	ret = append_nullable_uint64(buffer, event->bytes_total_valid,
		event->bytes_total);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"files_done\":");
	APPEND_UINT(event->files_done);
	APPEND_TEXT(",\"files_total\":");
	ret = append_nullable_uint64(buffer, event->files_total_valid,
		event->files_total);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"rate_bytes_per_second\":");
	APPEND_UINT(event->rate_bytes_per_second);
	APPEND_TEXT(",\"queue_fill_bytes\":");
	APPEND_UINT(event->queue_fill_bytes);
	APPEND_TEXT(",\"buffer_underrun_count\":");
	APPEND_UINT(event->buffer_underrun_count);
	APPEND_TEXT(",\"retry_count\":");
	APPEND_UINT(event->retry_count);
	APPEND_TEXT(",\"memory_high_water_bytes\":");
	APPEND_UINT(event->memory_high_water_bytes);
	APPEND_TEXT(",\"telemetry_overflowed\":");
	APPEND_TEXT(event->telemetry_overflowed ? "true" : "false");
	APPEND_TEXT(",\"phase_elapsed_ns\":");
	APPEND_UINT(event->phase_elapsed_ns);
	APPEND_TEXT(",\"index_done\":");
	ret = append_nullable_uint64(buffer, event->index_progress_valid,
		event->index_done);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"index_total\":");
	ret = append_nullable_uint64(buffer, event->index_progress_valid,
		event->index_total);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"eta_seconds\":");
	ret = append_nullable_uint64(buffer, event->eta_seconds_valid,
		event->eta_seconds);
	if (ret < 0)
		return ret;
	APPEND_TEXT(",\"device_serial\":");
	if (event->device_serial)
		APPEND_STRING(event->device_serial);
	else
		APPEND_TEXT("null");
	APPEND_TEXT(",\"message_code\":");
	APPEND_STRING(event->message_code);
	APPEND_TEXT("}\n");
#undef APPEND_TEXT
#undef APPEND_STRING
#undef APPEND_UINT
	return 0;
}

static int write_complete(int fd, const char *buffer, size_t length)
{
	sigset_t blocked;
	sigset_t old_mask;
	sigset_t pending;
	bool sigpipe_was_pending = false;
	size_t offset = 0;
	int mask_ret;
	int result = 0;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGPIPE);
	mask_ret = pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);
	if (mask_ret != 0)
		return -mask_ret;
	if (sigpending(&pending) < 0) {
		result = -errno;
		goto restore_mask;
	}
	sigpipe_was_pending = sigismember(&pending, SIGPIPE) == 1;
	while (offset < length) {
		ssize_t written = LTFS_EVENTS_WRITE(fd, buffer + offset,
			length - offset);
		if (written > 0) {
			offset += (size_t)written;
			continue;
		}
		if (written < 0 && errno == EINTR)
			continue;
		result = written == 0 ? -EIO : -errno;
		break;
	}
	if (result == -EPIPE && !sigpipe_was_pending) {
		struct timespec timeout = { 0, 0 };
		while (sigtimedwait(&blocked, NULL, &timeout) < 0 &&
		       errno == EINTR)
			;
	}

restore_mask:
	mask_ret = pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
	if (result == 0 && mask_ret != 0)
		result = -mask_ret;
	return result;
}

int ltfs_events_init(struct ltfs_event_sink *sink, int fd,
	const char *operation_id)
{
	int duplicate = -1;
	int flags;
	int ret;

	if (!sink || fd < -1 || !valid_uuid(operation_id))
		return -EINVAL;
	memset(sink, 0, sizeof(*sink));
	sink->fd = -1;
	ret = pthread_mutex_init(&sink->mutex, NULL);
	if (ret != 0)
		return -ret;
	if (fd >= 0) {
		duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 3);
		if (duplicate < 0) {
			ret = -errno;
			pthread_mutex_destroy(&sink->mutex);
			return ret;
		}
		flags = fcntl(duplicate, F_GETFL);
		if (flags < 0 || fcntl(duplicate, F_SETFL, flags | O_NONBLOCK) < 0) {
			ret = -errno;
			close(duplicate);
			pthread_mutex_destroy(&sink->mutex);
			return ret;
		}
	}
	sink->fd = duplicate;
	memcpy(sink->operation_id, operation_id, LTFS_EVENT_OPERATION_ID_SIZE);
	sink->initialized = true;
	return 0;
}

int ltfs_event_emit(struct ltfs_event_sink *sink,
	const struct ltfs_event *event)
{
	struct json_buffer buffer;
	struct timespec monotonic;
	struct timespec realtime;
	struct tm wall_tm;
	char wall_time[25];
	const char *phase;
	const char *status;
	uint64_t monotonic_ns;
	uint64_t next_seq;
	int ret;

	if (!sink || !event)
		return -EINVAL;
	buffer.data = sink->json_buffer;
	buffer.length = 0;
	buffer.capacity = sizeof(sink->json_buffer);
	ret = pthread_mutex_lock(&sink->mutex);
	if (ret != 0)
		return -ret;
	if (!sink->initialized) {
		ret = 0;
		goto unlock;
	}
	phase = phase_name(event->phase);
	status = status_name(event->status);
	if (!phase || !status || !valid_message_code(event->message_code) ||
	    (event->volume_uuid && !valid_uuid(event->volume_uuid)) ||
	    !valid_utf8(event->device_serial, LTFS_EVENT_MAX_DEVICE_SERIAL) ||
	    (event->index_progress_valid &&
	     (event->index_total == 0 || event->index_done > event->index_total))) {
		ret = -EINVAL;
		goto unlock;
	}
	if (sink->fd < 0) {
		ret = 0;
		goto unlock;
	}
	if (sink->seq == UINT64_MAX) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &monotonic) < 0 ||
	    clock_gettime(CLOCK_REALTIME, &realtime) < 0) {
		ret = -errno;
		goto unlock;
	}
	if (!gmtime_r(&realtime.tv_sec, &wall_tm)) {
		ret = -errno;
		goto unlock;
	}
	ret = snprintf(wall_time, sizeof(wall_time),
		"%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
		wall_tm.tm_year + 1900, wall_tm.tm_mon + 1, wall_tm.tm_mday,
		wall_tm.tm_hour, wall_tm.tm_min, wall_tm.tm_sec,
		realtime.tv_nsec / 1000000L);
	if (ret != (int)sizeof(wall_time) - 1) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	monotonic_ns = (uint64_t)monotonic.tv_sec * UINT64_C(1000000000) +
		(uint64_t)monotonic.tv_nsec;
	next_seq = sink->seq + 1;
	ret = append_event_json(&buffer, sink, event, next_seq,
		monotonic_ns, wall_time);
	if (ret < 0)
		goto unlock;
	ret = write_complete(sink->fd, buffer.data, buffer.length);
	if (ret == 0)
		sink->seq = next_seq;
	else {
		close(sink->fd);
		sink->fd = -1;
	}

unlock:
	pthread_mutex_unlock(&sink->mutex);
	return ret;
}

void ltfs_events_close(struct ltfs_event_sink *sink)
{
	if (!sink)
		return;
	if (pthread_mutex_lock(&sink->mutex) != 0)
		return;
	if (sink->initialized) {
		if (sink->fd >= 0)
			close(sink->fd);
		sink->fd = -1;
		sink->initialized = false;
	}
	pthread_mutex_unlock(&sink->mutex);
}
