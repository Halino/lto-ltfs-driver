/* SPDX-License-Identifier: BSD-3-Clause */

#include "finalization.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

static uint64_t finalization_now(struct ltfs_finalization *finalization)
{
	struct timespec now;

	if (finalization->ops.monotonic_ns)
		return finalization->ops.monotonic_ns(finalization->context);
	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
}

static void copy_identifier(char destination[LTFS_FINALIZATION_ID_SIZE],
	const char *source)
{
	if (!source) {
		destination[0] = '\0';
		return;
	}
	strncpy(destination, source, LTFS_FINALIZATION_ID_SIZE - 1);
	destination[LTFS_FINALIZATION_ID_SIZE - 1] = '\0';
}

static void emit_event(struct ltfs_finalization *finalization,
	enum ltfs_finalization_phase phase,
	enum ltfs_finalization_event_status status, const char *message_code,
	int result, uint64_t elapsed_ns, bool index_progress_valid,
	uint64_t index_done, uint64_t index_total)
{
	struct ltfs_finalization_event event;
	int ret;

	if (!finalization->ops.emit)
		return;
	memset(&event, 0, sizeof(event));
	copy_identifier(event.operation_id,
		finalization->receipt.operation_id);
	copy_identifier(event.volume_uuid,
		finalization->receipt.volume_uuid);
	event.prior_generation = finalization->receipt.prior_generation;
	event.new_generation = finalization->receipt.new_generation;
	event.phase = phase;
	event.status = status;
	event.message_code = message_code;
	event.result = result;
	event.device_close_result_valid =
		finalization->receipt.device_close_result_valid;
	event.device_close_result = finalization->receipt.device_close_result;
	event.phase_elapsed_ns = elapsed_ns;
	event.bytes_valid = finalization->receipt.bytes_valid;
	event.bytes = finalization->receipt.bytes;
	event.files_valid = finalization->receipt.files_valid;
	event.files = finalization->receipt.files;
	event.rate_bytes_per_second = 0;
	event.index_progress_valid = index_progress_valid;
	event.index_done = index_done;
	event.index_total = index_total;
	ret = finalization->ops.emit(finalization->context, &event);
	if (ret != 0 && finalization->event_result == 0)
		finalization->event_result = ret;
}

static uint64_t finish_current_phase(struct ltfs_finalization *finalization,
	enum ltfs_finalization_event_status status, const char *message_code,
	int result)
{
	uint64_t now = finalization_now(finalization);
	uint64_t elapsed = now >= finalization->phase_started_ns ?
		now - finalization->phase_started_ns : 0;

	if (finalization->phase > LTFS_FINALIZATION_NONE &&
	    finalization->phase < LTFS_FINALIZATION_PHASE_COUNT)
		finalization->receipt.phase_duration_ns[finalization->phase] +=
			elapsed;
	if (status == LTFS_FINALIZATION_EVENT_COMPLETE &&
	    finalization->phase_message_code)
		message_code = finalization->phase_message_code;
	emit_event(finalization, finalization->phase, status, message_code,
		result, elapsed, false, 0, 0);
	finalization->phase_open = false;
	return now;
}

static void account_current_phase(struct ltfs_finalization *finalization)
{
	uint64_t now;
	uint64_t elapsed;

	if (!finalization->phase_open)
		return;
	now = finalization_now(finalization);
	elapsed = now >= finalization->phase_started_ns ?
		now - finalization->phase_started_ns : 0;
	if (finalization->phase > LTFS_FINALIZATION_NONE &&
	    finalization->phase < LTFS_FINALIZATION_PHASE_COUNT)
		finalization->receipt.phase_duration_ns[finalization->phase] +=
			elapsed;
	finalization->phase_open = false;
}

static int start_phase(struct ltfs_finalization *finalization,
	enum ltfs_finalization_phase phase, const char *message_code)
{
	if (phase <= LTFS_FINALIZATION_NONE ||
	    phase >= LTFS_FINALIZATION_PHASE_COUNT)
		return -EINVAL;
	finalization->phase = phase;
	finalization->phase_message_code = message_code;
	finalization->phase_started_ns = finalization_now(finalization);
	finalization->phase_open = true;
	emit_event(finalization, phase, LTFS_FINALIZATION_EVENT_STARTED,
		message_code, 0, 0, false, 0, 0);
	return 0;
}

int ltfs_finalization_init(struct ltfs_finalization *finalization,
	const struct ltfs_finalization_config *config)
{
	if (!finalization || !config || !config->operation_id ||
	    strlen(config->operation_id) >= LTFS_FINALIZATION_ID_SIZE ||
	    (config->volume_uuid &&
	     strlen(config->volume_uuid) >= LTFS_FINALIZATION_ID_SIZE))
		return -EINVAL;
	memset(finalization, 0, sizeof(*finalization));
	finalization->context = config->context;
	finalization->ops = config->ops;
	finalization->capture_enabled = config->capture_enabled;
	finalization->unload_enabled = config->unload_enabled;
	copy_identifier(finalization->receipt.operation_id,
		config->operation_id);
	copy_identifier(finalization->receipt.volume_uuid,
		config->volume_uuid);
	finalization->receipt.prior_generation = config->prior_generation;
	finalization->receipt.new_generation = config->new_generation;
	finalization->receipt.bytes_valid = config->bytes_valid;
	finalization->receipt.bytes = config->bytes_valid ? config->bytes : 0;
	finalization->receipt.files_valid = config->files_valid;
	finalization->receipt.files = config->files_valid ? config->files : 0;
	return 0;
}

int ltfs_finalization_advance(struct ltfs_finalization *finalization,
	enum ltfs_finalization_phase phase, const char *message_code)
{
	if (!finalization || !message_code)
		return -EINVAL;
	if (phase != LTFS_FINALIZATION_WRITING_INDEX &&
	    phase != LTFS_FINALIZATION_UNMOUNTING)
		return -EINVAL;
	if (finalization->phase != LTFS_FINALIZATION_NONE &&
	    phase <= finalization->phase)
		return -EINVAL;
	if (finalization->phase != LTFS_FINALIZATION_NONE)
		finish_current_phase(finalization,
			LTFS_FINALIZATION_EVENT_COMPLETE, message_code, 0);
	return start_phase(finalization, phase, message_code);
}

int ltfs_finalization_index_progress(struct ltfs_finalization *finalization,
	uint64_t done, bool total_valid, uint64_t total)
{
	uint64_t now;
	uint64_t elapsed;

	if (!finalization || !total_valid || total == 0)
		return -EINVAL;
	if (done > total)
		return -ERANGE;
	if (finalization->phase != LTFS_FINALIZATION_WRITING_INDEX)
		return -EINVAL;
	now = finalization_now(finalization);
	elapsed = now >= finalization->phase_started_ns ?
		now - finalization->phase_started_ns : 0;
	emit_event(finalization, LTFS_FINALIZATION_WRITING_INDEX,
		LTFS_FINALIZATION_EVENT_PROGRESS, "finalization.index.progress",
		0, elapsed, true, done, total);
	return 0;
}

void ltfs_finalization_set_receipt(struct ltfs_finalization *finalization,
	uint64_t new_generation, bool bytes_valid, uint64_t bytes,
	bool files_valid, uint64_t files,
	const char *volume_uuid)
{
	if (!finalization)
		return;
	finalization->receipt.new_generation = new_generation;
	finalization->receipt.bytes_valid = bytes_valid;
	finalization->receipt.bytes = bytes_valid ? bytes : 0;
	finalization->receipt.files_valid = files_valid;
	finalization->receipt.files = files_valid ? files : 0;
	if (volume_uuid && strlen(volume_uuid) < LTFS_FINALIZATION_ID_SIZE)
		copy_identifier(finalization->receipt.volume_uuid, volume_uuid);
}

void ltfs_finalization_set_media_identity(struct ltfs_finalization *finalization,
	uint64_t new_generation, const char *volume_uuid)
{
	if (!finalization)
		return;
	finalization->receipt.new_generation = new_generation;
	if (volume_uuid && strlen(volume_uuid) < LTFS_FINALIZATION_ID_SIZE)
		copy_identifier(finalization->receipt.volume_uuid, volume_uuid);
}

static int fail_finalization(struct ltfs_finalization *finalization,
	int result, bool cleanup_failed, const char *message_code)
{
	/* Emit exactly one terminal failure; only account the interrupted phase. */
	account_current_phase(finalization);
	finalization->receipt.result = result;
	finalization->receipt.cleanup_failed = cleanup_failed;
	finalization->phase = LTFS_FINALIZATION_FAILED;
	finalization->phase_started_ns = finalization_now(finalization);
	emit_event(finalization, LTFS_FINALIZATION_FAILED,
		LTFS_FINALIZATION_EVENT_FAILED, message_code, result, 0, false,
		0, 0);
	finalization->finished = true;
	finalization->running = false;
	return result;
}

static int call_step(int (*callback)(void *), void *context)
{
	return callback ? callback(context) : 0;
}

static void precommit_cleanup(struct ltfs_finalization *finalization)
{
	if (!finalization->scheduler_destroy_attempted) {
		finalization->scheduler_destroy_attempted = true;
		call_step(finalization->ops.destroy_scheduler,
			finalization->context);
	}
	if (!finalization->kmi_destroy_attempted) {
		finalization->kmi_destroy_attempted = true;
		call_step(finalization->ops.destroy_kmi, finalization->context);
	}
	if (!finalization->device_close_attempted) {
		uint64_t started = finalization_now(finalization);
		int close_result;

		finalization->device_close_attempted = true;
		close_result = finalization->ops.close_device ?
			finalization->ops.close_device(finalization->context) :
			-ENOSYS;
		finalization->receipt.device_close_result_valid = true;
		finalization->receipt.device_close_result = close_result;
		finalization->receipt.device_close_duration_ns =
			finalization_now(finalization) - started;
	}
}

static bool receipt_ack_matches(const struct ltfs_commit_receipt *receipt,
	const struct ltfs_commit_ack *ack)
{
	return ack->durable &&
		ack->generation == receipt->new_generation &&
		strcmp(ack->operation_id, receipt->operation_id) == 0 &&
		strcmp(ack->volume_uuid, receipt->volume_uuid) == 0;
}

static int persist_receipt(struct ltfs_finalization *finalization)
{
	struct ltfs_commit_ack ack;
	uint64_t started;
	int ret;

	ret = start_phase(finalization, LTFS_FINALIZATION_PERSISTING_RECEIPT,
		"finalization.catalog.receipt");
	if (ret != 0)
		return ret;
	if (!finalization->receipt.volume_uuid[0] ||
	    finalization->receipt.new_generation == 0)
		return -ENODATA;
	started = finalization_now(finalization);
	memset(&ack, 0, sizeof(ack));
	if (!finalization->ops.persist_receipt)
		ret = -ENOTCONN;
	else {
		finalization->prepared_receipt = finalization->receipt;
		finalization->prepared_receipt_valid = true;
		ret = finalization->ops.persist_receipt(finalization->context,
			&finalization->receipt, &ack);
	}
	finalization->receipt.catalog_ack_duration_ns =
		finalization_now(finalization) - started;
	if (ret != 0)
		return ret;
	if (!receipt_ack_matches(&finalization->receipt, &ack))
		return -EPROTO;
	finalization->receipt.catalog_acknowledged = true;
	finish_current_phase(finalization, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.catalog.receipt", 0);
	return 0;
}

static int finalize_receipt(struct ltfs_finalization *finalization)
{
	if (!finalization->receipt.catalog_acknowledged ||
		!finalization->prepared_receipt_valid ||
		!finalization->ops.finalize_receipt)
		return 0;
	return finalization->ops.finalize_receipt(finalization->context,
		&finalization->prepared_receipt, &finalization->receipt);
}

static int run_phase_step(struct ltfs_finalization *finalization,
	enum ltfs_finalization_phase phase, const char *message_code,
	int (*callback)(void *))
{
	int ret = start_phase(finalization, phase, message_code);

	if (ret != 0)
		return ret;
	ret = call_step(callback, finalization->context);
	if (ret != 0)
		return ret;
	finish_current_phase(finalization, LTFS_FINALIZATION_EVENT_COMPLETE,
		message_code, 0);
	return 0;
}

int ltfs_finalize(struct ltfs_finalization *finalization,
	struct ltfs_commit_receipt *receipt)
{
	uint64_t started;
	int ret;
	int cleanup_result = 0;

	if (!finalization || !receipt)
		return -EINVAL;
	if (finalization->running || finalization->finished)
		return -EALREADY;
	finalization->running = true;

	ret = run_phase_step(finalization, LTFS_FINALIZATION_CLOSING_HANDLES,
		"finalization.handles", finalization->ops.drain_handles);
	if (ret != 0)
		goto precommit_failure;

	ret = start_phase(finalization, LTFS_FINALIZATION_DRAINING_DATA,
		"finalization.data.drain");
	if (ret != 0)
		goto precommit_failure;
	ret = call_step(finalization->ops.flush_data, finalization->context);
	if (ret != 0)
		goto precommit_failure;
	finalization->scheduler_destroy_attempted = true;
	ret = call_step(finalization->ops.destroy_scheduler,
		finalization->context);
	if (ret != 0)
		goto precommit_failure;
	finalization->kmi_destroy_attempted = true;
	ret = call_step(finalization->ops.destroy_kmi, finalization->context);
	if (ret != 0)
		goto precommit_failure;
	finish_current_phase(finalization, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.data.drain", 0);

	ret = start_phase(finalization, LTFS_FINALIZATION_BUILDING_INDEX,
		"finalization.index.build");
	if (ret < 0)
		goto precommit_failure;
	if (!finalization->ops.commit_unmount) {
		ret = -ENOSYS;
		goto precommit_failure;
	}
	ret = finalization->ops.commit_unmount(finalization->context,
		finalization);
	if (ret != 0)
		goto precommit_failure;
	if (finalization->phase == LTFS_FINALIZATION_BUILDING_INDEX) {
		ret = ltfs_finalization_advance(finalization,
			LTFS_FINALIZATION_UNMOUNTING, "finalization.unmount");
		if (ret < 0)
			goto precommit_failure;
	} else if (finalization->phase == LTFS_FINALIZATION_WRITING_INDEX) {
		ret = ltfs_finalization_advance(finalization,
			LTFS_FINALIZATION_UNMOUNTING, "finalization.unmount");
		if (ret < 0)
			goto precommit_failure;
	}
	finish_current_phase(finalization, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.unmount", 0);
	finalization->receipt.media_committed = true;
	start_phase(finalization, LTFS_FINALIZATION_MEDIA_COMMITTED,
		"finalization.media.committed");
	finish_current_phase(finalization, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.media.committed", 0);

	if (finalization->capture_enabled && finalization->ops.capture_index) {
		started = finalization_now(finalization);
		ret = finalization->ops.capture_index(finalization->context);
		finalization->receipt.capture_duration_ns =
			finalization_now(finalization) - started;
		if (ret != 0) {
			cleanup_result = ret;
			finalization->receipt.cleanup_failed = true;
			finalization->receipt.result = ret;
		}
	}
	ret = persist_receipt(finalization);
	if (ret != 0) {
		account_current_phase(finalization);
		if (cleanup_result == 0)
			cleanup_result = ret;
	}
	if (finalization->unload_enabled &&
	    finalization->receipt.catalog_acknowledged) {
		if (!finalization->ops.unload_media) {
			ret = start_phase(finalization, LTFS_FINALIZATION_UNLOADING,
				"finalization.media.unload");
			if (ret == 0)
				ret = -ENOSYS;
		} else
			ret = run_phase_step(finalization,
				LTFS_FINALIZATION_UNLOADING,
				"finalization.media.unload",
				finalization->ops.unload_media);
		if (ret != 0) {
			account_current_phase(finalization);
			if (cleanup_result == 0)
				cleanup_result = ret;
		}
	}
	if (cleanup_result == 0)
		ret = start_phase(finalization, LTFS_FINALIZATION_COMPLETE,
			"finalization.complete");
	started = finalization_now(finalization);
	finalization->device_close_attempted = true;
	ret = finalization->ops.close_device ?
		finalization->ops.close_device(finalization->context) : -ENOSYS;
	finalization->receipt.device_close_result_valid = true;
	finalization->receipt.device_close_result = ret;
	finalization->receipt.device_close_duration_ns =
		finalization_now(finalization) - started;
	if (ret != 0 && cleanup_result == 0)
		cleanup_result = ret;
	if (finalization->event_result != 0 && cleanup_result == 0)
		cleanup_result = finalization->event_result;
	if (cleanup_result != 0)
		goto cleanup_failure;
	finish_current_phase(finalization, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.complete", 0);
	if (finalization->event_result != 0) {
		cleanup_result = finalization->event_result;
		goto cleanup_failure;
	}
	finalization->receipt.result = 0;
	ret = finalize_receipt(finalization);
	if (ret != 0) {
		cleanup_result = ret;
		goto cleanup_failure;
	}
	finalization->finished = true;
	finalization->running = false;
	*receipt = finalization->receipt;
	return 0;

precommit_failure:
	account_current_phase(finalization);
	precommit_cleanup(finalization);
	ret = fail_finalization(finalization, ret, false,
		"finalization.content.failed");
	*receipt = finalization->receipt;
	return ret;

cleanup_failure:
	ret = fail_finalization(finalization, cleanup_result, true,
		"finalization.cleanup.failed");
	(void)finalize_receipt(finalization);
	*receipt = finalization->receipt;
	return ret;
}
