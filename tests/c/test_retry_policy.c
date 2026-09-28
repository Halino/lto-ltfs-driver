/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "retry_policy.h"
#ifndef LTFS_RETRY_POLICY_ONLY
#include "tape_drivers/linux/sg/sg_scsi_tape.h"
#endif

#include <errno.h>
#include <scsi/sg.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef LTFS_RETRY_POLICY_ONLY
#include "libltfs/ltfs_error.h"
#include "tape_drivers/spc_op_codes.h"
#include "tape_drivers/ssc_op_codes.h"
#endif

struct decision_case {
	const char *name;
	struct ltfs_retry_input input;
	enum ltfs_retry_action action;
	uint32_t delay_ms;
	bool refresh_identity;
	const char *message_code;
};

static int test_decision_table(void)
{
	static const struct decision_case cases[] = {
		{
			"ready-first-backoff",
			{LTFS_CMD_READ_ONLY_INFO, 0x02, 0x04, 0x01, 0, 0, 7750, false, false, false},
			LTFS_RETRY_AFTER_MS, 250, false, "scsi.retry.becoming_ready"
		},
		{
			"ready-last-backoff",
			{LTFS_CMD_READ_ONLY_INFO, 0x02, 0x04, 0x01, 4, 3750, 7750, false, false, false},
			LTFS_RETRY_AFTER_MS, 4000, false, "scsi.retry.becoming_ready"
		},
		{
			"ready-deadline-cap",
			{LTFS_CMD_READ_ONLY_INFO, 0x02, 0x04, 0x01, 4, 7400, 7750, false, false, false},
			LTFS_RETRY_AFTER_MS, 350, false, "scsi.retry.becoming_ready"
		},
		{
			"unit-attention-refresh-once",
			{LTFS_CMD_POSITIONING, 0x06, 0x28, 0x00, 0, 0, 7750, false, false, false},
			LTFS_RETRY_AFTER_MS, 250, true, "scsi.retry.unit_attention"
		},
		{
			"unit-attention-second-is-permanent",
			{LTFS_CMD_POSITIONING, 0x06, 0x28, 0x00, 1, 250, 7750, false, true, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.unit_attention_repeated"
		},
		{
			"first-unit-attention-after-other-retry-refreshes",
			{LTFS_CMD_POSITIONING, 0x06, 0x28, 0x00, 1, 250, 7750, false, false, false},
			LTFS_RETRY_AFTER_MS, 500, true, "scsi.retry.unit_attention"
		},
		{
			"recovered-error-is-success",
			{LTFS_CMD_IDEMPOTENT, 0x01, 0x11, 0x00, 0, 0, 7750, false, false, true},
			LTFS_RETRY_SUCCESS, 0, false, "scsi.success.recovered"
		},
		{
			"key1-special-status-preserves-upstream-result",
			{LTFS_CMD_IDEMPOTENT, 0x01, 0x00, 0x17,
				0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false,
			"scsi.fail.recovered_status_preserved"
		},
		{
			"write-protect-is-permanent",
			{LTFS_CMD_WRITE, 0x07, 0x27, 0x00, 0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.write_protected"
		},
		{
			"medium-error-is-permanent",
			{LTFS_CMD_READ_ONLY_INFO, 0x03, 0x11, 0x00, 0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.medium_error"
		},
		{
			"hardware-error-is-permanent",
			{LTFS_CMD_POSITIONING, 0x04, 0x44, 0x00, 0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.hardware_error"
		},
		{
			"illegal-request-is-permanent",
			{LTFS_CMD_IDEMPOTENT, 0x05, 0x24, 0x00, 0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.illegal_request"
		},
		{
			"aborted-before-data-out-retries",
			{LTFS_CMD_WRITE, 0x0b, 0x47, 0x00, 0, 0, 7750, false, false, false},
			LTFS_RETRY_AFTER_MS, 250, false, "scsi.retry.aborted_command"
		},
		{
			"aborted-data-out-is-ambiguous",
			{LTFS_CMD_WRITE, 0x0b, 0x47, 0x00, 0, 0, 7750, true, false, false},
			LTFS_STOP_AMBIGUOUS, 0, false, "scsi.stop.ambiguous_write"
		},
		{
			"commit-unknown-data-out-is-ambiguous",
			{LTFS_CMD_COMMIT, 0x0f, 0xff, 0xff, 0, 0, 7750, true, false, false},
			LTFS_STOP_AMBIGUOUS, 0, false, "scsi.stop.ambiguous_write"
		},
		{
			"relative-position-abort-is-ambiguous",
			{(enum ltfs_retry_command_class)6, 0x0b, 0x47, 0x00,
				0, 0, 7750, true, false, false},
			LTFS_STOP_AMBIGUOUS, 0, false,
			"scsi.stop.ambiguous_position"
		},
		{
			"relative-position-medium-error-remains-ambiguous",
			{(enum ltfs_retry_command_class)6, 0x03, 0x11, 0x00,
				0, 0, 7750, true, false, false},
			LTFS_STOP_AMBIGUOUS, 0, false,
			"scsi.stop.ambiguous_position"
		},
		{
			"state-change-abort-is-ambiguous",
			{(enum ltfs_retry_command_class)7, 0x0b, 0x47, 0x00,
				0, 0, 7750, true, false, false},
			LTFS_STOP_AMBIGUOUS, 0, false,
			"scsi.stop.ambiguous_state_change"
		},
		{
			"relative-position-unit-attention-is-not-replayed",
			{(enum ltfs_retry_command_class)6, 0x06, 0x28, 0x00,
				0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false,
			"scsi.fail.non_idempotent_retry_blocked"
		},
		{
			"state-change-becoming-ready-is-not-replayed",
			{(enum ltfs_retry_command_class)7, 0x02, 0x04, 0x01,
				0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false,
			"scsi.fail.non_idempotent_retry_blocked"
		},
		{
			"unknown-operation-unit-attention-is-not-replayed",
			{(enum ltfs_retry_command_class)8, 0x06, 0x28, 0x00,
				0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false,
			"scsi.fail.non_idempotent_retry_blocked"
		},
		{
			"deadline-exhausted",
			{LTFS_CMD_READ_ONLY_INFO, 0x02, 0x04, 0x01, 5, 7750, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.retry_deadline"
		},
		{
			"unknown-sense-is-permanent",
			{LTFS_CMD_READ_ONLY_INFO, 0x0f, 0xfe, 0xfd, 0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.unknown_sense"
		},
		{
			"invalid-command-class-is-rejected",
			{(enum ltfs_retry_command_class)99, 0x01, 0x00, 0x00, 0, 0, 7750, false, false, false},
			LTFS_FAIL_PERMANENT, 0, false, "scsi.fail.invalid_retry_input"
		},
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct ltfs_retry_decision actual = ltfs_retry_classify(&cases[index].input);
		if (actual.action != cases[index].action ||
			actual.delay_ms != cases[index].delay_ms ||
			actual.refresh_identity != cases[index].refresh_identity ||
			strcmp(actual.message_code, cases[index].message_code) != 0) {
			fprintf(stderr, "decision case failed: %s\n", cases[index].name);
			return 1;
		}
	}
	return 0;
}

#ifndef LTFS_RETRY_POLICY_ONLY
extern enum ltfs_retry_command_class sg_retry_command_classify(
	uint8_t operation, int transfer_direction);
typedef void (*test_retry_observer_fn)(const char *message_code,
	enum ltfs_retry_action action, uint8_t operation, void *opaque);
extern void sg_set_retry_observer(struct sg_tape *device,
	test_retry_observer_fn observer, void *opaque);
extern uint64_t sg_get_recovered_error_count(const struct sg_tape *device);

static int test_retry_clock_is_deterministic(void)
{
	struct ltfs_timespec now = {1, 1};
	CHECK_INT_EQ(get_unix_current_timespec(&now), 0);
	CHECK_INT_EQ(now.tv_sec, 0);
	CHECK_INT_EQ(now.tv_nsec, 0);
	return 0;
}

static int test_sg_opcode_safety_map(void)
{
	static const struct {
		uint8_t operation;
		int direction;
		enum ltfs_retry_command_class expected;
	} cases[] = {
		{INQUIRY, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{LOG_SENSE, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{MAINTENANCE_IN, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{MODE_SENSE6, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{MODE_SENSE10, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{PERSISTENT_RESERVE_IN, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{READ_ATTRIBUTE, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{READ_BLOCK_LIMITS, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{READ_BUFFER, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{READ_DYNAMIC_RUNTIME_ATTRIBUTE, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{READ_POSITION, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{RECEIVE_DIAGNOSTIC_RESULTS, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{REPORT_DENSITY_SUPPORT, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{REPORT_LUNS, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{REQUEST_SENSE, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{TEST_UNIT_READY, SCSI_NO_DATA_TRANSFER, LTFS_CMD_READ_ONLY_INFO},
		{THIRD_PARTY_COPY_IN, SCSI_FROM_TARGET_TO_INITIATOR, LTFS_CMD_READ_ONLY_INFO},
		{LOCATE10, SCSI_NO_DATA_TRANSFER, LTFS_CMD_POSITIONING},
		{LOCATE16, SCSI_NO_DATA_TRANSFER, LTFS_CMD_POSITIONING},
		{REWIND, SCSI_NO_DATA_TRANSFER, LTFS_CMD_POSITIONING},
		{SPACE6, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)6},
		{SPACE16, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)6},
		{READ, SCSI_FROM_TARGET_TO_INITIATOR,
			(enum ltfs_retry_command_class)6},
		{READ_REVERSE, SCSI_FROM_TARGET_TO_INITIATOR,
			(enum ltfs_retry_command_class)6},
		{RECOVER_BUFFERED_DATA, SCSI_FROM_TARGET_TO_INITIATOR,
			(enum ltfs_retry_command_class)6},
		{STRING_SEARCH, SCSI_FROM_TARGET_TO_INITIATOR,
			(enum ltfs_retry_command_class)6},
		{WRITE, SCSI_FROM_INITIATOR_TO_TARGET, LTFS_CMD_WRITE},
		{WRITE_ATTRIBUTE, SCSI_FROM_INITIATOR_TO_TARGET, LTFS_CMD_WRITE},
		{WRITE_DYNAMIC_RUNTIME_ATTRIBUTE, SCSI_FROM_INITIATOR_TO_TARGET, LTFS_CMD_WRITE},
		{WRITE_FILEMARKS6, SCSI_NO_DATA_TRANSFER, LTFS_CMD_COMMIT},
		{SET_CAPACITY, SCSI_NO_DATA_TRANSFER, LTFS_CMD_DESTRUCTIVE},
		{ALLOW_OVERWRITE, SCSI_FROM_INITIATOR_TO_TARGET,
			LTFS_CMD_DESTRUCTIVE},
		{ERASE, SCSI_NO_DATA_TRANSFER, LTFS_CMD_DESTRUCTIVE},
		{FORMAT_MEDIUM, SCSI_FROM_INITIATOR_TO_TARGET,
			LTFS_CMD_DESTRUCTIVE},
		{CHANGE_DEFINITION, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{DISPLAY_MESSAGE, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{LOAD_UNLOAD, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{LOG_SELECT, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{MAINTENANCE_OUT, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{MODE_SELECT6, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{MODE_SELECT10, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{PERSISTENT_RESERVE_OUT, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{PREVENT_ALLOW_MEDIUM_REMOVAL, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{RELEASE_UNIT6, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{RELEASE_UNIT10, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{RESERVE_UNIT6, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{RESERVE_UNIT10, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{SEND_DIAGNOSTIC, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{SPIN, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{SPOUT, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{VERIFY, SCSI_NO_DATA_TRANSFER,
			(enum ltfs_retry_command_class)7},
		{WRITE_BUFFER, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{XCOPY, SCSI_FROM_INITIATOR_TO_TARGET,
			(enum ltfs_retry_command_class)7},
		{0xff, SCSI_FROM_TARGET_TO_INITIATOR,
			(enum ltfs_retry_command_class)8},
	};
	size_t index;

	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
		CHECK_INT_EQ(sg_retry_command_classify(cases[index].operation,
			cases[index].direction), cases[index].expected);
	return 0;
}

enum fixture_kind {
	FIXTURE_GOOD,
	FIXTURE_SENSE,
	FIXTURE_HOST_RETRY,
	FIXTURE_TARGET_UNEXPECTED,
	FIXTURE_DRIVER_SENSE_NONE,
	FIXTURE_DRIVER_SENSE_RETRY,
	FIXTURE_DRIVER_SENSE_ABORT,
	FIXTURE_DRIVER_SENSE_REMAP,
	FIXTURE_DRIVER_SENSE_DIE,
	FIXTURE_DRIVER_SENSE_SENSE,
	FIXTURE_INQUIRY_STANDARD,
	FIXTURE_INQUIRY_SERIAL,
	FIXTURE_INQUIRY_SERIAL_OVERSIZE,
	FIXTURE_INQUIRY_SERIAL_SHORT,
};

struct ioctl_fixture {
	enum fixture_kind kind;
	uint8_t sense_key;
	uint8_t asc;
	uint8_t ascq;
	int resid;
};

static struct ioctl_fixture fixtures[16];
static size_t fixture_count;
static size_t fixture_cursor;
static uint64_t fake_now_ms;
static unsigned int slept_ms[16];
static size_t sleep_count;
static unsigned int identity_refresh_count;
static int identity_refresh_result;
static uint64_t identity_refresh_advance_ms;
static const char *fake_inquiry_serial;
static unsigned int observed_retry_events;
static enum ltfs_retry_action observed_action;
static const char *observed_message_code;

static void observe_retry_event(const char *message_code,
	enum ltfs_retry_action action, uint8_t operation, void *opaque)
{
	(void)operation;
	(void)opaque;
	++observed_retry_events;
	observed_action = action;
	observed_message_code = message_code;
}

int ltfs_log_level = -1;
int ltfs_syslog_level = -1;
bool ltfs_print_thread_id = false;

int ltfsmsg_internal(bool print_id, int level, char **msg_out,
	const char *id, ...)
{
	(void)print_id;
	(void)level;
	(void)msg_out;
	(void)id;
	return 0;
}

uint64_t ltfs_sg_test_monotonic_ms(void)
{
	return fake_now_ms;
}

int ltfs_sg_test_sleep(unsigned int milliseconds)
{
	if (sleep_count >= sizeof(slept_ms) / sizeof(slept_ms[0]))
		return -1;
	slept_ms[sleep_count++] = milliseconds;
	fake_now_ms += milliseconds;
	return 0;
}

int ltfs_sg_test_ioctl(int fd, unsigned long request, void *argument)
{
	sg_io_hdr_t *io = argument;
	struct ioctl_fixture fixture;
	(void)fd;
	if (request != SG_IO || fixture_cursor >= fixture_count) {
		errno = EIO;
		return -1;
	}
	fixture = fixtures[fixture_cursor++];
	io->host_status = 0;
	io->driver_status = 0;
	io->status = 0;
	io->masked_status = 0;
	io->sb_len_wr = 0;
	io->resid = fixture.resid;
	if (fixture.kind == FIXTURE_INQUIRY_STANDARD) {
		unsigned char *buffer = io->dxferp;
		if (io->cmdp[0] != INQUIRY || io->cmdp[2] != 0)
			return -1;
		memset(buffer, 0, io->dxfer_len);
		buffer[0] = SEQUENTIAL_DEVICE;
		memcpy(buffer + 8, "FAKEVEND", 8);
		memcpy(buffer + 16, "FAKE TAPE       ", 16);
		memcpy(buffer + 32, "0001", 4);
		return 0;
	}
	if (fixture.kind == FIXTURE_INQUIRY_SERIAL) {
		unsigned char *buffer = io->dxferp;
		size_t length = strlen(fake_inquiry_serial);
		if (io->cmdp[0] != INQUIRY || io->cmdp[2] != 0x80 || length > 0xff)
			return -1;
		memset(buffer, 0, io->dxfer_len);
		buffer[1] = 0x80;
		buffer[3] = (unsigned char)length;
		memcpy(buffer + 4, fake_inquiry_serial, length);
		return 0;
	}
	if (fixture.kind == FIXTURE_INQUIRY_SERIAL_OVERSIZE) {
		unsigned char *buffer = io->dxferp;
		if (io->cmdp[0] != INQUIRY || io->cmdp[2] != 0x80)
			return -1;
		memset(buffer, 0, io->dxfer_len);
		buffer[1] = 0x80;
		buffer[2] = 0x01;
		buffer[3] = 0x00;
		return 0;
	}
	if (fixture.kind == FIXTURE_INQUIRY_SERIAL_SHORT) {
		unsigned char *buffer = io->dxferp;
		if (io->cmdp[0] != INQUIRY || io->cmdp[2] != 0x80)
			return -1;
		memset(buffer, 0, io->dxfer_len);
		buffer[1] = 0x80;
		buffer[3] = 9;
		return 0;
	}
	if (fixture.kind == FIXTURE_GOOD)
		return 0;
	if (fixture.kind == FIXTURE_HOST_RETRY) {
		io->host_status = 0x0c;
		return 0;
	}
	if (fixture.kind == FIXTURE_TARGET_UNEXPECTED) {
		io->masked_status = 0x7f;
		return 0;
	}
	memset(io->sbp, 0, io->mx_sb_len);
	io->sbp[0] = 0x70;
	io->sbp[2] = fixture.sense_key;
	io->sbp[12] = fixture.asc;
	io->sbp[13] = fixture.ascq;
	io->sb_len_wr = 14;
	io->masked_status = 0x01;
	switch (fixture.kind) {
		case FIXTURE_DRIVER_SENSE_NONE:
			io->driver_status = 0x08;
			break;
		case FIXTURE_DRIVER_SENSE_RETRY:
			io->driver_status = 0x18;
			break;
		case FIXTURE_DRIVER_SENSE_ABORT:
			io->driver_status = 0x28;
			break;
		case FIXTURE_DRIVER_SENSE_REMAP:
			io->driver_status = 0x38;
			break;
		case FIXTURE_DRIVER_SENSE_DIE:
			io->driver_status = 0x48;
			break;
		case FIXTURE_DRIVER_SENSE_SENSE:
			io->driver_status = 0x88;
			break;
		default:
			break;
	}
	return 0;
}

static int fake_refresh_identity(struct sg_tape *device, void *opaque)
{
	(void)device;
	(void)opaque;
	++identity_refresh_count;
	fake_now_ms += identity_refresh_advance_ms;
	return identity_refresh_result;
}

static void reset_script(const struct ioctl_fixture *script, size_t count)
{
	memset(fixtures, 0, sizeof(fixtures));
	memcpy(fixtures, script, count * sizeof(script[0]));
	fixture_count = count;
	fixture_cursor = 0;
	fake_now_ms = 0;
	memset(slept_ms, 0, sizeof(slept_ms));
	sleep_count = 0;
	identity_refresh_count = 0;
	identity_refresh_result = 0;
	identity_refresh_advance_ms = 0;
	fake_inquiry_serial = "SERIAL-A";
	observed_retry_events = 0;
	observed_action = LTFS_FAIL_PERMANENT;
	observed_message_code = NULL;
}

static void init_request(sg_io_hdr_t *request, unsigned char *cdb,
	unsigned char *sense, int direction, unsigned int transfer_length)
{
	memset(request, 0, sizeof(*request));
	memset(sense, 0, MAXSENSE);
	request->interface_id = 'S';
	request->dxfer_direction = direction;
	request->cmd_len = CDB6_LEN;
	request->mx_sb_len = MAXSENSE;
	request->dxfer_len = transfer_length;
	request->cmdp = cdb;
	request->sbp = sense;
	request->timeout = LTFS_SG_RETRY_DEADLINE_MS;
	request->usr_ptr = (void *)"fixture";
}

static int test_sg_becoming_ready_has_exact_attempts(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x02, 0x04, 0x01, 0},
		{FIXTURE_SENSE, 0x02, 0x04, 0x01, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 3);
	CHECK_INT_EQ(sleep_count, 2);
	CHECK_INT_EQ(slept_ms[0], 250);
	CHECK_INT_EQ(slept_ms[1], 500);
	return 0;
}

static int test_sg_unit_attention_refreshes_once(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {
		.fd = 7,
		.refresh_identity = fake_refresh_identity,
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 2);
	CHECK_INT_EQ(identity_refresh_count, 1);
	CHECK_INT_EQ(sleep_count, 1);
	return 0;
}

static int test_sg_unit_attention_is_not_retried_twice(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {
		.fd = 7,
		.refresh_identity = fake_refresh_identity,
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_TRUE(sg_issue_cdb_command(&device, &request, &message) < 0);
	CHECK_INT_EQ(fixture_cursor, 2);
	CHECK_INT_EQ(identity_refresh_count, 1);
	CHECK_INT_EQ(sleep_count, 1);
	return 0;
}

static int test_sg_mixed_retry_then_first_unit_attention(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x02, 0x04, 0x01, 0},
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {
		.fd = 7,
		.refresh_identity = fake_refresh_identity,
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 3);
	CHECK_INT_EQ(identity_refresh_count, 1);
	CHECK_INT_EQ(sleep_count, 2);
	return 0;
}

static int test_sg_slow_identity_refresh_consumes_deadline(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {
		.fd = 7,
		.refresh_identity = fake_refresh_identity,
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	identity_refresh_advance_ms = LTFS_SG_RETRY_DEADLINE_MS;
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_TIMEOUT);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(identity_refresh_count, 1);
	CHECK_INT_EQ(sleep_count, 0);
	CHECK_STR_EQ(message, "scsi.fail.retry_deadline");
	return 0;
}

static int test_sg_refresh_failure_prevents_replay(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {
		.fd = 7,
		.refresh_identity = fake_refresh_identity,
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	identity_refresh_result = -EIO;
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_TRUE(sg_issue_cdb_command(&device, &request, &message) < 0);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(identity_refresh_count, 1);
	CHECK_INT_EQ(sleep_count, 0);
	return 0;
}

static int test_sg_refresh_failure_causes_are_distinct(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	struct sg_tape device = {.fd = 7};

	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_IDENTITY_REFRESH_MISSING);
	CHECK_STR_EQ(message, "scsi.fail.identity_refresh_unavailable");

	device.refresh_identity = fake_refresh_identity;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	identity_refresh_result = -EDEV_IDENTITY_MISMATCH;
	message = NULL;
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_IDENTITY_MISMATCH);
	CHECK_STR_EQ(message, "scsi.fail.identity_mismatch");

	reset_script(script, sizeof(script) / sizeof(script[0]));
	identity_refresh_result = -EIO;
	message = NULL;
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_IDENTITY_REFRESH_ERROR);
	CHECK_STR_EQ(message, "scsi.fail.identity_refresh_error");
	return 0;
}

static int test_sg_driver_sense_is_classified_before_retry_hint(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_DRIVER_SENSE_RETRY, 0x06, 0x28, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {
		.fd = 7,
		.refresh_identity = fake_refresh_identity,
	};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 2);
	CHECK_INT_EQ(identity_refresh_count, 1);
	CHECK_INT_EQ(sleep_count, 1);
	return 0;
}

static int test_sg_all_driver_sense_suggestions_preserve_sense(void)
{
	static struct error_table table[] = {
		{0x031100, -EDEV_READ_PERM, "upstream driver-sense mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const enum fixture_kind kinds[] = {
		FIXTURE_DRIVER_SENSE_NONE,
		FIXTURE_DRIVER_SENSE_RETRY,
		FIXTURE_DRIVER_SENSE_ABORT,
		FIXTURE_DRIVER_SENSE_REMAP,
		FIXTURE_DRIVER_SENSE_DIE,
		FIXTURE_DRIVER_SENSE_SENSE,
	};
	size_t index;
	standard_table = table;
	vendor_table = table;
	for (index = 0; index < sizeof(kinds) / sizeof(kinds[0]); ++index) {
		struct ioctl_fixture script[] = {
			{kinds[index], 0x03, 0x11, 0x00, 0},
		};
		struct sg_tape device = {.fd = 7};
		sg_io_hdr_t request;
		unsigned char cdb[CDB6_LEN] = {INQUIRY};
		unsigned char sense[MAXSENSE];
		char *message = NULL;
		reset_script(script, sizeof(script) / sizeof(script[0]));
		init_request(&request, cdb, sense, SCSI_FROM_TARGET_TO_INITIATOR, 96);
		CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
			-EDEV_READ_PERM);
		CHECK_INT_EQ(fixture_cursor, 1);
		CHECK_STR_EQ(message, "upstream driver-sense mapping");
	}
	return 0;
}

static int test_sg_bound_identity_refresh_compares_serial(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_INQUIRY_STANDARD, 0, 0, 0, 0},
		{FIXTURE_INQUIRY_SERIAL, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	scsi_device_identifier identity;
	memset(&identity, 0, sizeof(identity));
	strcpy(identity.unit_serial, "SERIAL-A");
	CHECK_INT_EQ(sg_bind_drive_identity(&device, &identity), 0);
	CHECK_TRUE(device.refresh_identity != NULL);
	reset_script(script, sizeof(script) / sizeof(script[0]));
	CHECK_INT_EQ(device.refresh_identity(&device,
		device.refresh_identity_opaque), 0);
	CHECK_INT_EQ(fixture_cursor, 2);
	reset_script(script, sizeof(script) / sizeof(script[0]));
	fake_inquiry_serial = "SERIAL-B";
	CHECK_INT_EQ(device.refresh_identity(&device,
		device.refresh_identity_opaque), -EDEV_IDENTITY_MISMATCH);
	CHECK_INT_EQ(fixture_cursor, 2);
	return 0;
}

static int test_sg_vpd80_rejects_declared_length_past_buffer(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_INQUIRY_STANDARD, 0, 0, 0, 0},
		{FIXTURE_INQUIRY_SERIAL_OVERSIZE, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	scsi_device_identifier identity;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	memset(&identity, 0xa5, sizeof(identity));
	CHECK_INT_EQ(sg_get_drive_identifier(&device, &identity),
		-EDEV_LENGTH_MISMATCH);
	CHECK_INT_EQ(fixture_cursor, 2);
	{
		static const struct ioctl_fixture short_script[] = {
			{FIXTURE_INQUIRY_STANDARD, 0, 0, 0, 0},
			{FIXTURE_INQUIRY_SERIAL_SHORT, 0, 0, 0,
				MAX_INQ_LEN - 12},
		};
		reset_script(short_script,
			sizeof(short_script) / sizeof(short_script[0]));
		CHECK_INT_EQ(sg_get_drive_identifier(&device, &identity),
			-EDEV_LENGTH_MISMATCH);
		CHECK_INT_EQ(fixture_cursor, 2);
	}
	return 0;
}

static int test_sg_unit_attention_uses_bound_identity_refresh(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x06, 0x28, 0x00, 0},
		{FIXTURE_INQUIRY_STANDARD, 0, 0, 0, 0},
		{FIXTURE_INQUIRY_SERIAL, 0, 0, 0, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	scsi_device_identifier identity;
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {TEST_UNIT_READY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	memset(&identity, 0, sizeof(identity));
	strcpy(identity.unit_serial, "SERIAL-A");
	CHECK_INT_EQ(sg_bind_drive_identity(&device, &identity), 0);
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 4);
	CHECK_INT_EQ(sleep_count, 1);
	return 0;
}

static int test_sg_write_ambiguity_is_never_replayed(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x0b, 0x47, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {WRITE};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_FROM_INITIATOR_TO_TARGET, 1024);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_WRITE_AMBIGUOUS);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(sleep_count, 0);
	CHECK_STR_EQ(message, "scsi.stop.ambiguous_write");
	return 0;
}

static int test_sg_proven_zero_transfer_can_retry(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x0b, 0x47, 0x00, 1024},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {WRITE};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_FROM_INITIATOR_TO_TARGET, 1024);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 2);
	CHECK_INT_EQ(sleep_count, 1);
	return 0;
}

static int test_sg_relative_space_is_never_replayed(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x0b, 0x47, 0x00, 0},
		{FIXTURE_GOOD, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {SPACE6};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_WRITE_AMBIGUOUS);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(sleep_count, 0);
	CHECK_STR_EQ(message, "scsi.stop.ambiguous_position");
	return 0;
}

static int test_sg_commit_like_commands_are_never_replayed(void)
{
	static const uint8_t operations[] = {
		WRITE_FILEMARKS6, SET_CAPACITY, ALLOW_OVERWRITE,
	};
	size_t index;
	for (index = 0; index < sizeof(operations) / sizeof(operations[0]); ++index) {
		static const struct ioctl_fixture script[] = {
			{FIXTURE_HOST_RETRY, 0, 0, 0, 0},
			{FIXTURE_GOOD, 0, 0, 0, 0},
		};
		struct sg_tape device = {.fd = 7};
		sg_io_hdr_t request;
		unsigned char cdb[CDB6_LEN] = {0};
		unsigned char sense[MAXSENSE];
		char *message = NULL;
		cdb[0] = operations[index];
		reset_script(script, sizeof(script) / sizeof(script[0]));
		init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
		CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
			-EDEV_WRITE_AMBIGUOUS);
		CHECK_INT_EQ(fixture_cursor, 1);
		CHECK_INT_EQ(sleep_count, 0);
	}
	return 0;
}

static int test_sg_generic_host_retry_is_bounded(void)
{
	struct ioctl_fixture script[7];
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {INQUIRY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	size_t index;
	for (index = 0; index < sizeof(script) / sizeof(script[0]); ++index)
		script[index] = (struct ioctl_fixture){FIXTURE_HOST_RETRY, 0, 0, 0, 0};
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_FROM_TARGET_TO_INITIATOR, 96);
	CHECK_TRUE(sg_issue_cdb_command(&device, &request, &message) < 0);
	CHECK_INT_EQ(fixture_cursor, 5);
	CHECK_INT_EQ(sleep_count, 5);
	CHECK_INT_EQ(fake_now_ms, LTFS_SG_RETRY_DEADLINE_MS);
	return 0;
}

static int test_sg_preserves_upstream_sense_mapping(void)
{
	static struct error_table table[] = {
		{0x031100, -EDEV_READ_PERM, "upstream medium mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x03, 0x11, 0x00, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {INQUIRY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	standard_table = table;
	vendor_table = table;
	init_request(&request, cdb, sense, SCSI_FROM_TARGET_TO_INITIATOR, 96);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), -EDEV_READ_PERM);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_STR_EQ(message, "upstream medium mapping");
	return 0;
}

static int test_sg_read_driver_sense_filemark_is_not_ambiguous(void)
{
	static struct error_table table[] = {
		{0x000001, -EDEV_FILEMARK_DETECTED, "upstream filemark mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct ioctl_fixture script[] = {
		{FIXTURE_DRIVER_SENSE_NONE, SK_FM_SET, 0x00, 0x01, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {READ};
	unsigned char sense[MAXSENSE];
	char *message = NULL;

	reset_script(script, sizeof(script) / sizeof(script[0]));
	standard_table = table;
	vendor_table = table;
	init_request(&request, cdb, sense, SCSI_FROM_TARGET_TO_INITIATOR, 96);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_FILEMARK_DETECTED);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(sleep_count, 0);
	CHECK_STR_EQ(message, "upstream filemark mapping");
	return 0;
}

static int test_sg_read_driver_sense_ili_is_not_ambiguous(void)
{
	static struct error_table table[] = {
		{0x000000, -EDEV_NO_SENSE, "upstream no sense mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct ioctl_fixture script[] = {
		{FIXTURE_DRIVER_SENSE_NONE, SK_ILI_SET, 0x00, 0x00, 64},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {READ};
	unsigned char sense[MAXSENSE];
	char *message = NULL;

	reset_script(script, sizeof(script) / sizeof(script[0]));
	standard_table = table;
	vendor_table = table;
	init_request(&request, cdb, sense, SCSI_FROM_TARGET_TO_INITIATOR, 96);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_NO_SENSE);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(sleep_count, 0);
	CHECK_STR_EQ(message, "upstream no sense mapping");
	return 0;
}

static int test_sg_sequential_terminal_sense_is_not_ambiguous(void)
{
	static struct error_table table[] = {
		{0x000000, -EDEV_NO_SENSE, "upstream no sense mapping"},
		{0x000002, -EDEV_EARLY_WARNING, "upstream early warning"},
		{0x000007, -EDEV_PROG_EARLY_WARNING, "upstream prog warning"},
		{0x000017, -EDEV_CLEANING_REQUIRED, "upstream cleaning warning"},
		{0x080005, -EDEV_EOD_DETECTED, "upstream eod mapping"},
		{0x081401, -EDEV_RECORD_NOT_FOUND, "upstream record mapping"},
		{0x081403, -EDEV_EOD_NOT_FOUND, "upstream eod-not-found mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct {
		uint8_t operation;
		uint8_t sense_key;
		uint8_t asc;
		uint8_t ascq;
		int direction;
		int expected;
	} cases[] = {
		{READ, SK_FM_SET, 0x00, 0x00, SCSI_FROM_TARGET_TO_INITIATOR,
			-EDEV_NO_SENSE},
		{READ, 0x08, 0x00, 0x05, SCSI_FROM_TARGET_TO_INITIATOR,
			-EDEV_EOD_DETECTED},
		{READ, 0x08, 0x14, 0x01, SCSI_FROM_TARGET_TO_INITIATOR,
			-EDEV_RECORD_NOT_FOUND},
		{READ, 0x08, 0x14, 0x03, SCSI_FROM_TARGET_TO_INITIATOR,
			-EDEV_EOD_NOT_FOUND},
		{READ, 0x00, 0x00, 0x17, SCSI_FROM_TARGET_TO_INITIATOR,
			-EDEV_CLEANING_REQUIRED},
		{WRITE, 0x00, 0x00, 0x02, SCSI_FROM_INITIATOR_TO_TARGET,
			-EDEV_EARLY_WARNING},
		{WRITE_FILEMARKS6, 0x00, 0x00, 0x07, SCSI_NO_DATA_TRANSFER,
			-EDEV_PROG_EARLY_WARNING},
	};
	size_t index;

	standard_table = table;
	vendor_table = table;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct ioctl_fixture script[] = {
			{FIXTURE_DRIVER_SENSE_NONE, cases[index].sense_key,
				cases[index].asc, cases[index].ascq, 0},
		};
		struct sg_tape device = {.fd = 7};
		sg_io_hdr_t request;
		unsigned char cdb[CDB6_LEN] = {0};
		unsigned char sense[MAXSENSE];
		char *message = NULL;

		cdb[0] = cases[index].operation;
		reset_script(script, sizeof(script) / sizeof(script[0]));
		init_request(&request, cdb, sense, cases[index].direction, 96);
		CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
			cases[index].expected);
		CHECK_INT_EQ(fixture_cursor, 1);
		CHECK_INT_EQ(sleep_count, 0);
	}
	return 0;
}

static int test_sg_unqualified_no_sense_stays_ambiguous(void)
{
	static struct error_table table[] = {
		{0x000000, -EDEV_NO_SENSE, "upstream no sense mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct {
		uint8_t operation;
		uint8_t sense_key;
	} cases[] = {
		{READ, 0x00},
		{SPACE6, SK_ILI_SET},
	};
	size_t index;

	standard_table = table;
	vendor_table = table;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct ioctl_fixture script[] = {
			{FIXTURE_DRIVER_SENSE_NONE, cases[index].sense_key, 0x00, 0x00, 0},
		};
		struct sg_tape device = {.fd = 7};
		sg_io_hdr_t request;
		unsigned char cdb[CDB6_LEN] = {0};
		unsigned char sense[MAXSENSE];
		char *message = NULL;

		cdb[0] = cases[index].operation;
		reset_script(script, sizeof(script) / sizeof(script[0]));
		init_request(&request, cdb, sense, SCSI_NO_DATA_TRANSFER, 0);
		CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
			-EDEV_WRITE_AMBIGUOUS);
		CHECK_INT_EQ(fixture_cursor, 1);
		CHECK_INT_EQ(sleep_count, 0);
		CHECK_STR_EQ(message, "scsi.stop.ambiguous_position");
	}
	return 0;
}

static int test_sg_recovered_success_is_counted_and_observable(void)
{
	static struct error_table table[] = {
		{0x011100, -EDEV_RECOVERED_ERROR, "upstream recovered mapping"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct ioctl_fixture script[] = {
		{FIXTURE_SENSE, 0x01, 0x11, 0x00, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {INQUIRY};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	standard_table = table;
	vendor_table = table;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	sg_set_retry_observer(&device, observe_retry_event, NULL);
	init_request(&request, cdb, sense, SCSI_FROM_TARGET_TO_INITIATOR, 96);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message), DEVICE_GOOD);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_INT_EQ(sg_get_recovered_error_count(&device), 1);
	CHECK_INT_EQ(observed_retry_events, 1);
	CHECK_INT_EQ(observed_action, LTFS_RETRY_SUCCESS);
	CHECK_STR_EQ(observed_message_code, "scsi.success.recovered");
	return 0;
}

static int test_sg_key1_special_statuses_preserve_upstream_mapping(void)
{
	static struct error_table table[] = {
		{0x010017, -EDEV_CLEANING_REQUIRED, "upstream cleaning warning"},
		{0x013700, -EDEV_MODE_PARAMETER_ROUNDED, "upstream rounded warning"},
		{0xffffff, -EDEV_UNKNOWN, "upstream unknown mapping"},
	};
	static const struct {
		uint8_t operation;
		uint8_t asc;
		uint8_t ascq;
		int direction;
		int expected;
		const char *message;
	} cases[] = {
		{READ, 0x00, 0x17, SCSI_FROM_TARGET_TO_INITIATOR,
			-EDEV_CLEANING_REQUIRED, "upstream cleaning warning"},
		{MODE_SELECT10, 0x37, 0x00, SCSI_FROM_INITIATOR_TO_TARGET,
			-EDEV_MODE_PARAMETER_ROUNDED, "upstream rounded warning"},
	};
	size_t index;
	standard_table = table;
	vendor_table = table;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct ioctl_fixture script[] = {
			{FIXTURE_SENSE, 0x01, cases[index].asc, cases[index].ascq, 0},
		};
		struct sg_tape device = {.fd = 7};
		sg_io_hdr_t request;
		unsigned char cdb[CDB10_LEN] = {0};
		unsigned char sense[MAXSENSE];
		char *message = NULL;
		cdb[0] = cases[index].operation;
		reset_script(script, sizeof(script) / sizeof(script[0]));
		init_request(&request, cdb, sense, cases[index].direction, 96);
		CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
			cases[index].expected);
		CHECK_INT_EQ(sg_get_recovered_error_count(&device), 0);
		CHECK_STR_EQ(message, cases[index].message);
	}
	return 0;
}

static int test_sg_unexpected_status_stops_ambiguous_write(void)
{
	static const struct ioctl_fixture script[] = {
		{FIXTURE_TARGET_UNEXPECTED, 0, 0, 0, 0},
	};
	struct sg_tape device = {.fd = 7};
	sg_io_hdr_t request;
	unsigned char cdb[CDB6_LEN] = {WRITE};
	unsigned char sense[MAXSENSE];
	char *message = NULL;
	reset_script(script, sizeof(script) / sizeof(script[0]));
	init_request(&request, cdb, sense, SCSI_FROM_INITIATOR_TO_TARGET, 1024);
	CHECK_INT_EQ(sg_issue_cdb_command(&device, &request, &message),
		-EDEV_WRITE_AMBIGUOUS);
	CHECK_INT_EQ(fixture_cursor, 1);
	CHECK_STR_EQ(message, "scsi.stop.ambiguous_write");
	return 0;
}
#endif

int main(void)
{
	if (test_decision_table() != 0
#ifndef LTFS_RETRY_POLICY_ONLY
		||
		test_retry_clock_is_deterministic() != 0 ||
		test_sg_opcode_safety_map() != 0 ||
		test_sg_becoming_ready_has_exact_attempts() != 0 ||
		test_sg_unit_attention_refreshes_once() != 0 ||
		test_sg_unit_attention_is_not_retried_twice() != 0 ||
		test_sg_mixed_retry_then_first_unit_attention() != 0 ||
		test_sg_slow_identity_refresh_consumes_deadline() != 0 ||
		test_sg_refresh_failure_prevents_replay() != 0 ||
		test_sg_refresh_failure_causes_are_distinct() != 0 ||
		test_sg_driver_sense_is_classified_before_retry_hint() != 0 ||
		test_sg_all_driver_sense_suggestions_preserve_sense() != 0 ||
		test_sg_bound_identity_refresh_compares_serial() != 0 ||
		test_sg_vpd80_rejects_declared_length_past_buffer() != 0 ||
		test_sg_unit_attention_uses_bound_identity_refresh() != 0 ||
		test_sg_write_ambiguity_is_never_replayed() != 0 ||
		test_sg_proven_zero_transfer_can_retry() != 0 ||
		test_sg_relative_space_is_never_replayed() != 0 ||
		test_sg_commit_like_commands_are_never_replayed() != 0 ||
		test_sg_generic_host_retry_is_bounded() != 0 ||
		test_sg_preserves_upstream_sense_mapping() != 0
		|| test_sg_read_driver_sense_filemark_is_not_ambiguous() != 0
		|| test_sg_read_driver_sense_ili_is_not_ambiguous() != 0
		|| test_sg_sequential_terminal_sense_is_not_ambiguous() != 0
		|| test_sg_unqualified_no_sense_stays_ambiguous() != 0
		|| test_sg_recovered_success_is_counted_and_observable() != 0
		|| test_sg_key1_special_statuses_preserve_upstream_mapping() != 0
		|| test_sg_unexpected_status_stops_ambiguous_write() != 0
#endif
	)
		return 1;
	puts("retry policy tests passed");
	return 0;
}
