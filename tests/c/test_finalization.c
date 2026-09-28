/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "finalization.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LEN(values) (sizeof(values) / sizeof((values)[0]))

enum scripted_step {
	STEP_HANDLES = 1,
	STEP_FLUSH,
	STEP_SCHEDULER,
	STEP_KMI,
	STEP_COMMIT,
	STEP_CAPTURE,
	STEP_PERSIST,
	STEP_UNLOAD,
	STEP_CLOSE,
	STEP_FINAL_RECEIPT,
};

struct fixture {
	struct ltfs_finalization finalization;
	struct ltfs_commit_receipt receipt;
	struct ltfs_commit_receipt persisted_receipt;
	struct ltfs_commit_receipt finalized_receipt;
	bool receipt_persisted;
	bool receipt_finalized;
	enum scripted_step calls[16];
	size_t call_count;
	struct ltfs_finalization_event events[32];
	size_t event_count;
	enum scripted_step fail_step;
	enum ltfs_finalization_phase emit_fail_phase;
	enum ltfs_finalization_event_status emit_fail_status;
	bool wrong_ack_operation;
	bool wrong_ack_generation;
	bool wrong_ack_volume;
	bool ack_not_durable;
	bool positive_close_result;
	int forced_close_result;
	bool skip_index_write;
	uint64_t now_ns;
};

static int record_step(struct fixture *fixture, enum scripted_step step)
{
	fixture->calls[fixture->call_count++] = step;
	fixture->now_ns += 10;
	return fixture->fail_step == step ? -EIO : 0;
}

static int drain_handles(void *context)
{
	return record_step(context, STEP_HANDLES);
}

static int flush_data(void *context)
{
	return record_step(context, STEP_FLUSH);
}

static int destroy_scheduler(void *context)
{
	return record_step(context, STEP_SCHEDULER);
}

static int destroy_kmi(void *context)
{
	return record_step(context, STEP_KMI);
}

static int commit_unmount(void *context, struct ltfs_finalization *finalization)
{
	struct fixture *fixture = context;
	int ret;

	ret = record_step(fixture, STEP_COMMIT);
	if (ret < 0)
		return ret;
	if (fixture->skip_index_write) {
		fixture->now_ns += 10;
		return ltfs_finalization_advance(finalization,
			LTFS_FINALIZATION_UNMOUNTING, "finalization.unmount");
	}
	ret = ltfs_finalization_advance(finalization,
		LTFS_FINALIZATION_WRITING_INDEX, "finalization.index.write");
	if (ret < 0)
		return ret;
	fixture->now_ns += 10;
	ret = ltfs_finalization_advance(finalization,
		LTFS_FINALIZATION_UNMOUNTING, "finalization.unmount");
	if (ret < 0)
		return ret;
	fixture->now_ns += 10;
	return 0;
}

static int capture_index(void *context)
{
	return record_step(context, STEP_CAPTURE);
}

static int persist_receipt(void *context,
	const struct ltfs_commit_receipt *receipt,
	struct ltfs_commit_ack *ack)
{
	struct fixture *fixture = context;
	int ret = record_step(fixture, STEP_PERSIST);

	if (ret < 0)
		return ret;
	fixture->persisted_receipt = *receipt;
	fixture->receipt_persisted = true;
	memset(ack, 0, sizeof(*ack));
	strncpy(ack->operation_id, receipt->operation_id,
		sizeof(ack->operation_id) - 1);
	strncpy(ack->volume_uuid, receipt->volume_uuid,
		sizeof(ack->volume_uuid) - 1);
	ack->generation = receipt->new_generation;
	ack->durable = !fixture->ack_not_durable;
	if (fixture->wrong_ack_operation)
		ack->operation_id[0] = '9';
	if (fixture->wrong_ack_volume)
		ack->volume_uuid[0] = '9';
	if (fixture->wrong_ack_generation)
		++ack->generation;
	return 0;
}

static int unload_media(void *context)
{
	return record_step(context, STEP_UNLOAD);
}

static int finalize_receipt(void *context,
	const struct ltfs_commit_receipt *prepared_receipt,
	const struct ltfs_commit_receipt *terminal_receipt)
{
	struct fixture *fixture = context;
	int ret = record_step(fixture, STEP_FINAL_RECEIPT);

	if (ret < 0)
		return ret;
	CHECK_TRUE(!prepared_receipt->catalog_acknowledged);
	CHECK_TRUE(!prepared_receipt->device_close_result_valid);
	fixture->finalized_receipt = *terminal_receipt;
	fixture->receipt_finalized = true;
	return 0;
}

static int close_device(void *context)
{
	struct fixture *fixture = context;
	int ret = record_step(context, STEP_CLOSE);

	if (ret != 0)
		return ret;
	if (fixture->forced_close_result != 0)
		return fixture->forced_close_result;
	return fixture->positive_close_result ? 7 : 0;
}

static uint64_t monotonic_ns(void *context)
{
	return ((struct fixture *)context)->now_ns;
}

static int emit_event(void *context,
	const struct ltfs_finalization_event *event)
{
	struct fixture *fixture = context;

	CHECK_TRUE(fixture->event_count < ARRAY_LEN(fixture->events));
	fixture->events[fixture->event_count++] = *event;
	if (fixture->emit_fail_phase == event->phase &&
	    fixture->emit_fail_status == event->status)
		return -EPIPE;
	return 0;
}

static int fixture_init(struct fixture *fixture, bool capture, bool unload)
{
	struct ltfs_finalization_config config;

	memset(fixture, 0, sizeof(*fixture));
	memset(&config, 0, sizeof(config));
	config.context = fixture;
	config.operation_id = "00000000-0000-4000-8000-000000000007";
	config.volume_uuid = "11111111-1111-4111-8111-111111111111";
	config.prior_generation = 17;
	config.new_generation = 18;
	config.bytes = 1234567;
	config.files = 42;
	config.bytes_valid = true;
	config.files_valid = true;
	config.capture_enabled = capture;
	config.unload_enabled = unload;
	config.ops.drain_handles = drain_handles;
	config.ops.flush_data = flush_data;
	config.ops.destroy_scheduler = destroy_scheduler;
	config.ops.destroy_kmi = destroy_kmi;
	config.ops.commit_unmount = commit_unmount;
	config.ops.capture_index = capture_index;
	config.ops.persist_receipt = persist_receipt;
	config.ops.finalize_receipt = finalize_receipt;
	config.ops.unload_media = unload_media;
	config.ops.close_device = close_device;
	config.ops.monotonic_ns = monotonic_ns;
	config.ops.emit = emit_event;
	CHECK_INT_EQ(ltfs_finalization_init(&fixture->finalization, &config), 0);
	return 0;
}

static bool event_seen(const struct fixture *fixture,
	enum ltfs_finalization_phase phase,
	enum ltfs_finalization_event_status status)
{
	size_t index;

	for (index = 0; index < fixture->event_count; ++index)
		if (fixture->events[index].phase == phase &&
		    fixture->events[index].status == status)
			return true;
	return false;
}

static bool event_seen_with_code(const struct fixture *fixture,
	enum ltfs_finalization_phase phase,
	enum ltfs_finalization_event_status status, const char *message_code)
{
	size_t index;

	for (index = 0; index < fixture->event_count; ++index)
		if (fixture->events[index].phase == phase &&
		    fixture->events[index].status == status &&
		    strcmp(fixture->events[index].message_code, message_code) == 0)
			return true;
	return false;
}

static bool call_seen(const struct fixture *fixture, enum scripted_step step)
{
	size_t index;

	for (index = 0; index < fixture->call_count; ++index)
		if (fixture->calls[index] == step)
			return true;
	return false;
}

static int test_success_order_and_receipt(void)
{
	static const enum scripted_step expected[] = {
		STEP_HANDLES, STEP_FLUSH, STEP_SCHEDULER, STEP_KMI,
		STEP_COMMIT, STEP_CAPTURE, STEP_PERSIST, STEP_UNLOAD, STEP_CLOSE,
		STEP_FINAL_RECEIPT,
	};
	struct fixture fixture;
	size_t index;

	CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), 0);
	CHECK_INT_EQ(fixture.call_count, ARRAY_LEN(expected));
	for (index = 0; index < ARRAY_LEN(expected); ++index)
		CHECK_INT_EQ(fixture.calls[index], expected[index]);
	CHECK_TRUE(fixture.receipt.media_committed);
	CHECK_TRUE(fixture.receipt.catalog_acknowledged);
	CHECK_INT_EQ(fixture.receipt.catalog_ack_duration_ns, 10);
	CHECK_TRUE(fixture.receipt.device_close_result_valid);
	CHECK_INT_EQ(fixture.receipt.device_close_result, 0);
	CHECK_TRUE(fixture.events[fixture.event_count - 1].device_close_result_valid);
	CHECK_INT_EQ(fixture.events[fixture.event_count - 1].device_close_result, 0);
	CHECK_TRUE(!fixture.receipt.cleanup_failed);
	CHECK_TRUE(fixture.receipt_finalized);
	CHECK_TRUE(fixture.finalized_receipt.catalog_acknowledged);
	CHECK_TRUE(fixture.finalized_receipt.device_close_result_valid);
	CHECK_INT_EQ(fixture.finalized_receipt.device_close_result, 0);
	CHECK_TRUE(!fixture.finalized_receipt.cleanup_failed);
	CHECK_INT_EQ(fixture.receipt.result, 0);
	CHECK_INT_EQ(fixture.receipt.prior_generation, 17);
	CHECK_INT_EQ(fixture.receipt.new_generation, 18);
	CHECK_INT_EQ(fixture.receipt.bytes, 1234567);
	CHECK_INT_EQ(fixture.receipt.files, 42);
	CHECK_TRUE(fixture.receipt.bytes_valid);
	CHECK_TRUE(fixture.receipt.files_valid);
	CHECK_STR_EQ(fixture.receipt.operation_id,
		"00000000-0000-4000-8000-000000000007");
	CHECK_STR_EQ(fixture.receipt.volume_uuid,
		"11111111-1111-4111-8111-111111111111");
	CHECK_TRUE(event_seen(&fixture, LTFS_FINALIZATION_MEDIA_COMMITTED,
		LTFS_FINALIZATION_EVENT_COMPLETE));
	CHECK_TRUE(event_seen(&fixture, LTFS_FINALIZATION_COMPLETE,
		LTFS_FINALIZATION_EVENT_COMPLETE));
	CHECK_STR_EQ(fixture.events[0].operation_id,
		"00000000-0000-4000-8000-000000000007");
	CHECK_STR_EQ(fixture.events[0].volume_uuid,
		"11111111-1111-4111-8111-111111111111");
	CHECK_INT_EQ(fixture.events[0].prior_generation, 17);
	CHECK_TRUE(event_seen_with_code(&fixture,
		LTFS_FINALIZATION_BUILDING_INDEX, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.index.build"));
	CHECK_TRUE(event_seen_with_code(&fixture,
		LTFS_FINALIZATION_WRITING_INDEX, LTFS_FINALIZATION_EVENT_COMPLETE,
		"finalization.index.write"));
	CHECK_TRUE(fixture.receipt.phase_duration_ns[LTFS_FINALIZATION_CLOSING_HANDLES] > 0);
	CHECK_TRUE(fixture.receipt.phase_duration_ns[LTFS_FINALIZATION_DRAINING_DATA] > 0);
	CHECK_TRUE(fixture.receipt.phase_duration_ns[LTFS_FINALIZATION_BUILDING_INDEX] > 0);
	CHECK_TRUE(fixture.receipt.phase_duration_ns[LTFS_FINALIZATION_WRITING_INDEX] > 0);
	CHECK_TRUE(fixture.receipt.phase_duration_ns[LTFS_FINALIZATION_UNMOUNTING] > 0);
	return 0;
}

static int test_fault_matrix(void)
{
	static const enum scripted_step failures[] = {
		STEP_HANDLES, STEP_FLUSH, STEP_SCHEDULER, STEP_KMI, STEP_COMMIT,
		STEP_CAPTURE, STEP_PERSIST, STEP_UNLOAD, STEP_CLOSE,
	};
	size_t index;

	for (index = 0; index < ARRAY_LEN(failures); ++index) {
		struct fixture fixture;
		int ret;

		CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
		fixture.fail_step = failures[index];
		ret = ltfs_finalize(&fixture.finalization, &fixture.receipt);
		CHECK_INT_EQ(ret, -EIO);
		CHECK_INT_EQ(fixture.receipt.result, -EIO);
		if (failures[index] <= STEP_COMMIT) {
			CHECK_TRUE(!fixture.receipt.media_committed);
			CHECK_TRUE(!fixture.receipt.cleanup_failed);
			CHECK_TRUE(!event_seen(&fixture,
				LTFS_FINALIZATION_MEDIA_COMMITTED,
				LTFS_FINALIZATION_EVENT_COMPLETE));
			CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));
			CHECK_TRUE(!call_seen(&fixture, STEP_CAPTURE));
			CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
		} else if (failures[index] == STEP_PERSIST) {
			CHECK_TRUE(fixture.receipt.media_committed);
			CHECK_TRUE(!fixture.receipt.catalog_acknowledged);
			CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
			CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));
		} else {
			CHECK_TRUE(fixture.receipt.media_committed);
			CHECK_TRUE(fixture.receipt.cleanup_failed);
			CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));
			CHECK_TRUE(fixture.receipt.catalog_acknowledged);
			CHECK_TRUE(event_seen(&fixture,
				LTFS_FINALIZATION_MEDIA_COMMITTED,
				LTFS_FINALIZATION_EVENT_COMPLETE));
		}
		CHECK_TRUE(!event_seen(&fixture, LTFS_FINALIZATION_COMPLETE,
			LTFS_FINALIZATION_EVENT_COMPLETE));
		CHECK_TRUE(event_seen(&fixture, LTFS_FINALIZATION_FAILED,
			LTFS_FINALIZATION_EVENT_FAILED));
	}
	return 0;
}

static int test_optional_and_partial_initialization(void)
{
	static const enum scripted_step expected[] = {
		STEP_HANDLES, STEP_FLUSH, STEP_SCHEDULER, STEP_KMI,
		STEP_COMMIT, STEP_PERSIST, STEP_CLOSE, STEP_FINAL_RECEIPT,
	};
	struct fixture fixture;
	struct ltfs_finalization_config config;
	struct ltfs_finalization finalization;
	struct ltfs_commit_receipt receipt;
	size_t index;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), 0);
	CHECK_INT_EQ(fixture.call_count, ARRAY_LEN(expected));
	for (index = 0; index < ARRAY_LEN(expected); ++index)
		CHECK_INT_EQ(fixture.calls[index], expected[index]);

	memset(&config, 0, sizeof(config));
	config.operation_id = "00000000-0000-4000-8000-000000000007";
	CHECK_INT_EQ(ltfs_finalization_init(&finalization, &config), 0);
	CHECK_INT_EQ(ltfs_finalize(&finalization, &receipt), -ENOSYS);
	CHECK_TRUE(!receipt.media_committed);
	CHECK_TRUE(!receipt.cleanup_failed);
	return 0;
}

static int test_ack_is_fail_closed_and_identity_bound(void)
{
	struct fixture fixture;
	struct ltfs_finalization_config config;
	struct ltfs_finalization finalization;
	struct ltfs_commit_receipt receipt;

	CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
	fixture.wrong_ack_operation = true;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-EPROTO);
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
	CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));

	CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
	fixture.wrong_ack_generation = true;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-EPROTO);
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));

	CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
	fixture.wrong_ack_volume = true;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-EPROTO);
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));

	CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
	fixture.ack_not_durable = true;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-EPROTO);
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));

	memset(&fixture, 0, sizeof(fixture));
	memset(&config, 0, sizeof(config));
	config.operation_id = "00000000-0000-4000-8000-000000000007";
	config.volume_uuid = "11111111-1111-4111-8111-111111111111";
	config.new_generation = 18;
	config.ops.commit_unmount = commit_unmount;
	config.ops.unload_media = unload_media;
	config.ops.close_device = close_device;
	config.ops.monotonic_ns = monotonic_ns;
	config.unload_enabled = true;
	config.context = &fixture;
	CHECK_INT_EQ(ltfs_finalization_init(&finalization, &config), 0);
	CHECK_INT_EQ(ltfs_finalize(&finalization, &receipt), -ENOTCONN);
	CHECK_TRUE(receipt.media_committed);
	CHECK_TRUE(!receipt.catalog_acknowledged);
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
	CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));

	memset(&fixture, 0, sizeof(fixture));
	config.context = &fixture;
	config.volume_uuid = "11111111-1111-4111-8111-111111111111";
	config.new_generation = 0;
	CHECK_INT_EQ(ltfs_finalization_init(&fixture.finalization, &config), 0);
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-ENODATA);
	CHECK_TRUE(!call_seen(&fixture, STEP_PERSIST));
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
	CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));
	return 0;
}

static int test_capture_failure_continues_cleanup_once(void)
{
	struct fixture fixture;
	size_t index;
	size_t failed_count = 0;

	CHECK_INT_EQ(fixture_init(&fixture, true, true), 0);
	fixture.fail_step = STEP_CAPTURE;
	fixture.forced_close_result = -EBUSY;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), -EIO);
	CHECK_TRUE(call_seen(&fixture, STEP_PERSIST));
	CHECK_TRUE(call_seen(&fixture, STEP_UNLOAD));
	CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));
	CHECK_TRUE(fixture.receipt.media_committed);
	CHECK_TRUE(fixture.receipt.catalog_acknowledged);
	CHECK_TRUE(fixture.receipt.cleanup_failed);
	CHECK_TRUE(fixture.receipt_persisted);
	CHECK_TRUE(fixture.persisted_receipt.media_committed);
	CHECK_TRUE(fixture.persisted_receipt.cleanup_failed);
	CHECK_INT_EQ(fixture.persisted_receipt.result, -EIO);
	CHECK_INT_EQ(fixture.receipt.capture_duration_ns, 10);
	CHECK_INT_EQ(fixture.receipt.device_close_duration_ns, 10);
	CHECK_TRUE(fixture.receipt.device_close_result_valid);
	CHECK_INT_EQ(fixture.receipt.device_close_result, -EBUSY);
	CHECK_TRUE(!event_seen(&fixture, LTFS_FINALIZATION_COMPLETE,
		LTFS_FINALIZATION_EVENT_COMPLETE));
	for (index = 0; index < fixture.event_count; ++index)
		if (fixture.events[index].status == LTFS_FINALIZATION_EVENT_FAILED) {
			++failed_count;
			CHECK_TRUE(fixture.events[index].device_close_result_valid);
			CHECK_INT_EQ(fixture.events[index].device_close_result, -EBUSY);
		}
	CHECK_INT_EQ(failed_count, 1);
	CHECK_TRUE(event_seen_with_code(&fixture, LTFS_FINALIZATION_FAILED,
		LTFS_FINALIZATION_EVENT_FAILED, "finalization.cleanup.failed"));
	return 0;
}

static int test_event_failure_is_never_success(void)
{
	static const enum ltfs_finalization_phase phases[] = {
		LTFS_FINALIZATION_CLOSING_HANDLES,
		LTFS_FINALIZATION_MEDIA_COMMITTED,
		LTFS_FINALIZATION_COMPLETE,
	};
	size_t index;

	for (index = 0; index < ARRAY_LEN(phases); ++index) {
		struct fixture fixture;

		CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
		fixture.emit_fail_phase = phases[index];
		fixture.emit_fail_status = phases[index] == LTFS_FINALIZATION_COMPLETE ?
			LTFS_FINALIZATION_EVENT_COMPLETE :
			LTFS_FINALIZATION_EVENT_STARTED;
		CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
			-EPIPE);
		CHECK_INT_EQ(fixture.receipt.result, -EPIPE);
	}
	return 0;
}

static int test_invalid_operation_counters_are_not_invented(void)
{
	struct fixture fixture;
	struct ltfs_finalization_config config;

	memset(&fixture, 0, sizeof(fixture));
	memset(&config, 0, sizeof(config));
	config.context = &fixture;
	config.operation_id = "00000000-0000-4000-8000-000000000007";
	config.volume_uuid = "11111111-1111-4111-8111-111111111111";
	config.new_generation = 18;
	config.bytes = UINT64_MAX;
	config.files = UINT64_MAX;
	config.ops.commit_unmount = commit_unmount;
	config.ops.persist_receipt = persist_receipt;
	config.ops.close_device = close_device;
	config.ops.monotonic_ns = monotonic_ns;
	config.ops.emit = emit_event;
	CHECK_INT_EQ(ltfs_finalization_init(&fixture.finalization, &config), 0);
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), 0);
	CHECK_TRUE(!fixture.receipt.bytes_valid);
	CHECK_TRUE(!fixture.receipt.files_valid);
	CHECK_INT_EQ(fixture.receipt.bytes, 0);
	CHECK_INT_EQ(fixture.receipt.files, 0);
	return 0;
}

static int test_nonzero_backend_close_result_is_observable(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	fixture.positive_close_result = true;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), 7);
	CHECK_INT_EQ(fixture.receipt.result, 7);
	CHECK_TRUE(fixture.receipt.cleanup_failed);
	CHECK_TRUE(fixture.receipt_finalized);
	CHECK_TRUE(fixture.finalized_receipt.media_committed);
	CHECK_TRUE(fixture.finalized_receipt.catalog_acknowledged);
	CHECK_TRUE(fixture.finalized_receipt.device_close_result_valid);
	CHECK_INT_EQ(fixture.finalized_receipt.device_close_result, 7);
	CHECK_TRUE(fixture.finalized_receipt.cleanup_failed);
	CHECK_INT_EQ(fixture.finalized_receipt.result, 7);
	CHECK_TRUE(!event_seen(&fixture, LTFS_FINALIZATION_COMPLETE,
		LTFS_FINALIZATION_EVENT_COMPLETE));
	return 0;
}

static int test_missing_volume_identity_cannot_be_acknowledged(void)
{
	struct fixture fixture;
	struct ltfs_finalization_config config;

	memset(&fixture, 0, sizeof(fixture));
	memset(&config, 0, sizeof(config));
	config.context = &fixture;
	config.operation_id = "00000000-0000-4000-8000-000000000007";
	config.new_generation = 18;
	config.ops.commit_unmount = commit_unmount;
	config.ops.persist_receipt = persist_receipt;
	config.ops.unload_media = unload_media;
	config.ops.close_device = close_device;
	config.ops.monotonic_ns = monotonic_ns;
	config.unload_enabled = true;
	CHECK_INT_EQ(ltfs_finalization_init(&fixture.finalization, &config), 0);
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-ENODATA);
	CHECK_TRUE(!call_seen(&fixture, STEP_PERSIST));
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
	CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));
	return 0;
}

static int test_required_cleanup_adapters_fail_closed(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, true), 0);
	fixture.finalization.ops.unload_media = NULL;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-ENOSYS);
	CHECK_TRUE(fixture.receipt.catalog_acknowledged);
	CHECK_TRUE(!call_seen(&fixture, STEP_UNLOAD));
	CHECK_TRUE(call_seen(&fixture, STEP_CLOSE));

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	fixture.finalization.ops.close_device = NULL;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt),
		-ENOSYS);
	CHECK_TRUE(fixture.receipt.catalog_acknowledged);
	CHECK_TRUE(!event_seen(&fixture, LTFS_FINALIZATION_COMPLETE,
		LTFS_FINALIZATION_EVENT_COMPLETE));
	return 0;
}

static int test_precommit_failure_retains_secondary_close_result(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	fixture.fail_step = STEP_HANDLES;
	fixture.forced_close_result = -EBUSY;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), -EIO);
	CHECK_INT_EQ(fixture.receipt.result, -EIO);
	CHECK_TRUE(fixture.receipt.device_close_result_valid);
	CHECK_INT_EQ(fixture.receipt.device_close_result, -EBUSY);
	CHECK_TRUE(event_seen(&fixture, LTFS_FINALIZATION_FAILED,
		LTFS_FINALIZATION_EVENT_FAILED));
	CHECK_TRUE(fixture.events[fixture.event_count - 1].device_close_result_valid);
	CHECK_INT_EQ(fixture.events[fixture.event_count - 1].device_close_result,
		-EBUSY);
	return 0;
}

static int test_clean_media_does_not_fabricate_index_write(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	fixture.skip_index_write = true;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), 0);
	CHECK_TRUE(!event_seen(&fixture, LTFS_FINALIZATION_WRITING_INDEX,
		LTFS_FINALIZATION_EVENT_STARTED));
	CHECK_TRUE(fixture.receipt.media_committed);
	return 0;
}

static int test_read_only_finalization_preserves_ready_generation(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	fixture.skip_index_write = true;
	fixture.finalization.receipt.new_generation =
		fixture.finalization.receipt.prior_generation;
	CHECK_INT_EQ(ltfs_finalize(&fixture.finalization, &fixture.receipt), 0);
	CHECK_INT_EQ(fixture.receipt.prior_generation, 17);
	CHECK_INT_EQ(fixture.receipt.new_generation, 17);
	CHECK_TRUE(fixture.receipt.media_committed);
	CHECK_TRUE(fixture.receipt.catalog_acknowledged);
	CHECK_TRUE(fixture.receipt_finalized);
	return 0;
}

static int test_index_progress_requires_real_total(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	CHECK_INT_EQ(ltfs_finalization_index_progress(&fixture.finalization,
		1, false, 0), -EINVAL);
	CHECK_INT_EQ(ltfs_finalization_index_progress(&fixture.finalization,
		11, true, 10), -ERANGE);
	CHECK_INT_EQ(ltfs_finalization_advance(&fixture.finalization,
		LTFS_FINALIZATION_WRITING_INDEX, "finalization.index.write"), 0);
	CHECK_INT_EQ(ltfs_finalization_index_progress(&fixture.finalization,
		5, true, 10), 0);
	CHECK_TRUE(fixture.events[fixture.event_count - 1].index_progress_valid);
	CHECK_INT_EQ(fixture.events[fixture.event_count - 1].index_done, 5);
	CHECK_INT_EQ(fixture.events[fixture.event_count - 1].index_total, 10);
	CHECK_INT_EQ(fixture.events[fixture.event_count - 1].rate_bytes_per_second, 0);
	return 0;
}

static int test_media_identity_update_preserves_operation_totals(void)
{
	struct fixture fixture;

	CHECK_INT_EQ(fixture_init(&fixture, false, false), 0);
	ltfs_finalization_set_media_identity(&fixture.finalization, 19,
		"11111111-2222-4333-8444-555555555555");
	CHECK_TRUE(fixture.finalization.receipt.bytes_valid);
	CHECK_INT_EQ(fixture.finalization.receipt.bytes, 1234567);
	CHECK_TRUE(fixture.finalization.receipt.files_valid);
	CHECK_INT_EQ(fixture.finalization.receipt.files, 42);
	CHECK_INT_EQ(fixture.finalization.receipt.new_generation, 19);
	CHECK_STR_EQ(fixture.finalization.receipt.volume_uuid,
		"11111111-2222-4333-8444-555555555555");
	return 0;
}

int main(void)
{
	CHECK_INT_EQ(test_success_order_and_receipt(), 0);
	CHECK_INT_EQ(test_fault_matrix(), 0);
	CHECK_INT_EQ(test_optional_and_partial_initialization(), 0);
	CHECK_INT_EQ(test_clean_media_does_not_fabricate_index_write(), 0);
	CHECK_INT_EQ(test_read_only_finalization_preserves_ready_generation(), 0);
	CHECK_INT_EQ(test_index_progress_requires_real_total(), 0);
	CHECK_INT_EQ(test_media_identity_update_preserves_operation_totals(), 0);
	CHECK_INT_EQ(test_ack_is_fail_closed_and_identity_bound(), 0);
	CHECK_INT_EQ(test_capture_failure_continues_cleanup_once(), 0);
	CHECK_INT_EQ(test_event_failure_is_never_success(), 0);
	CHECK_INT_EQ(test_invalid_operation_counters_are_not_invented(), 0);
	CHECK_INT_EQ(test_nonzero_backend_close_result_is_observable(), 0);
	CHECK_INT_EQ(test_missing_volume_identity_cannot_be_acknowledged(), 0);
	CHECK_INT_EQ(test_required_cleanup_adapters_fail_closed(), 0);
	CHECK_INT_EQ(test_precommit_failure_retains_secondary_close_result(), 0);
	puts("finalization tests passed");
	return 0;
}
