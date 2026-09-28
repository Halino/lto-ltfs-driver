/*
**
**  OO_Copyright_BEGIN
**
**
**  Copyright 2010, 2020 IBM Corp. All rights reserved.
**
**  Redistribution and use in source and binary forms, with or without
**   modification, are permitted provided that the following conditions
**  are met:
**  1. Redistributions of source code must retain the above copyright
**     notice, this list of conditions and the following disclaimer.
**  2. Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in the
**  documentation and/or other materials provided with the distribution.
**  3. Neither the name of the copyright holder nor the names of its
**     contributors may be used to endorse or promote products derived from
**     this software without specific prior written permission.
**
**  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
**  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
**  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
**  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
**  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
**  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
**  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
**  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
**  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
**  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
**  POSSIBILITY OF SUCH DAMAGE.
**
**
**  OO_Copyright_END
**
*************************************************************************************
**
** COMPONENT NAME:  IBM Linear Tape File System
**
** FILE NAME:       tape_drivers/linux/sg-ibmtape/sg_scsi_tape.c
**
** DESCRIPTION:     Implements SCSI command handling in sg tape driver
**
** AUTHORS:         Atsushi Abe
**                  IBM Tokyo Lab., Japan
**                  piste@jp.ibm.com
**
**
*************************************************************************************
*/

#include <stdint.h>
#include <sys/ioctl.h>
#include <time.h>

#include "libltfs/ltfs_error.h"
#include "libltfs/ltfslogging.h"
#include "libltfs/retry_policy.h"

#include "tape_drivers/vendor_compat.h"

#include "sg_scsi_tape.h"

#ifdef LTFS_SG_TESTING
extern int ltfs_sg_test_ioctl(int fd, unsigned long request, void *argument);
extern int ltfs_sg_test_sleep(unsigned int milliseconds);
extern uint64_t ltfs_sg_test_monotonic_ms(void);
#define LTFS_SG_IOCTL ltfs_sg_test_ioctl
#define LTFS_SG_SLEEP ltfs_sg_test_sleep
#define LTFS_SG_MONOTONIC_MS ltfs_sg_test_monotonic_ms
#else
#define LTFS_SG_IOCTL ioctl
#endif

/* Extern those 2 variables for sense conversion for specific tape */
struct error_table *standard_table = NULL;
struct error_table *vendor_table = NULL;

/* Local functions */
static int sg_refresh_drive_identity(struct sg_tape *device, void *opaque);

static int sg_sense2errno(sg_io_hdr_t *req, uint32_t *s, char **msg)
{
	int rc = -EDEV_UNKNOWN;
	unsigned char *sense = req->sbp;
	uint32_t sense_value = 0;

	unsigned char sk   = (*(sense + 2)) & 0x0F;
	unsigned char asc  = *(sense + 12);
	unsigned char ascq = *(sense + 13);

	sense_value += (uint32_t) sk << 16;
	sense_value += (uint32_t) asc << 8;
	sense_value += (uint32_t) ascq;

	*s = sense_value;

	rc = _sense2errorcode(sense_value, standard_table, msg, MASK_WITH_SENSE_KEY);
	/* NOTE: error table must be changed in library edition */
	if (rc == -EDEV_VENDOR_UNIQUE)
		rc = _sense2errorcode(sense_value, vendor_table, msg, MASK_WITH_SENSE_KEY);

	if (rc == -EDEV_UNKNOWN && ((sense_value & 0xFF0000) == 0x040000) )
		rc = -EDEV_HARDWARE_ERROR;

	if (rc == -EDEV_UNKNOWN) {
		ltfsmsg(LTFS_INFO, 30287I, sense_value);
	}

	return rc;
}

static uint64_t sg_monotonic_ms(void)
{
#ifdef LTFS_SG_TESTING
	return LTFS_SG_MONOTONIC_MS();
#else
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
#endif
}

static int sg_sleep_ms(unsigned int milliseconds)
{
#ifdef LTFS_SG_TESTING
	return LTFS_SG_SLEEP(milliseconds);
#else
	struct timespec request;
	struct timespec remainder;
	request.tv_sec = (time_t)(milliseconds / 1000U);
	request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
	while (nanosleep(&request, &remainder) != 0) {
		if (errno != EINTR)
			return -1;
		request = remainder;
	}
	return 0;
#endif
}

enum ltfs_retry_command_class sg_retry_command_classify(uint8_t operation,
	int transfer_direction)
{
	(void)transfer_direction;
	switch (operation) {
		case INQUIRY:
		case LOG_SENSE:
		case MAINTENANCE_IN:
		case MODE_SENSE6:
		case MODE_SENSE10:
		case PERSISTENT_RESERVE_IN:
		case READ_ATTRIBUTE:
		case READ_BLOCK_LIMITS:
		case READ_BUFFER:
		case READ_DYNAMIC_RUNTIME_ATTRIBUTE:
		case READ_POSITION:
		case RECEIVE_DIAGNOSTIC_RESULTS:
		case REPORT_DENSITY_SUPPORT:
		case REPORT_LUNS:
		case REQUEST_SENSE:
		case TEST_UNIT_READY:
		case THIRD_PARTY_COPY_IN:
			return LTFS_CMD_READ_ONLY_INFO;
		case LOCATE10:
		case LOCATE16:
		case REWIND:
			return LTFS_CMD_POSITIONING;
		case READ:
		case READ_REVERSE:
		case RECOVER_BUFFERED_DATA:
		case SPACE6:
		case SPACE16:
		case STRING_SEARCH:
			return LTFS_CMD_RELATIVE_POSITIONING;
		case WRITE:
		case WRITE_ATTRIBUTE:
		case WRITE_DYNAMIC_RUNTIME_ATTRIBUTE:
			return LTFS_CMD_WRITE;
		case WRITE_FILEMARKS6:
			return LTFS_CMD_COMMIT;
		case ALLOW_OVERWRITE:
		case ERASE:
		case FORMAT_MEDIUM:
		case SET_CAPACITY:
			return LTFS_CMD_DESTRUCTIVE;
		case CHANGE_DEFINITION:
		case DISPLAY_MESSAGE:
		case LOAD_UNLOAD:
		case LOG_SELECT:
		case MAINTENANCE_OUT:
		case MODE_SELECT6:
		case MODE_SELECT10:
		case PERSISTENT_RESERVE_OUT:
		case PREVENT_ALLOW_MEDIUM_REMOVAL:
		case RELEASE_UNIT6:
		case RELEASE_UNIT10:
		case RESERVE_UNIT6:
		case RESERVE_UNIT10:
		case SEND_DIAGNOSTIC:
		case SPIN:
		case SPOUT:
		case VERIFY:
		case WRITE_BUFFER:
		case XCOPY:
			return LTFS_CMD_STATE_CHANGING;
		default:
			return LTFS_CMD_UNKNOWN;
	}
}

static enum ltfs_retry_command_class sg_command_class(const sg_io_hdr_t *req)
{
	return sg_retry_command_classify(req->cmdp[0], req->dxfer_direction);
}

static bool sg_sense_proves_known_outcome(uint8_t sense_key)
{
	return sense_key == 0x01 || sense_key == 0x02 || sense_key == 0x05 ||
		sense_key == 0x06 || sense_key == 0x07;
}

static bool sg_operation_may_be_ambiguous(const sg_io_hdr_t *req,
	enum ltfs_retry_command_class command_class, bool sense_valid,
	uint8_t sense_key)
{
	if (command_class != LTFS_CMD_WRITE && command_class != LTFS_CMD_COMMIT &&
		command_class != LTFS_CMD_RELATIVE_POSITIONING &&
		command_class != LTFS_CMD_STATE_CHANGING &&
		command_class != LTFS_CMD_DESTRUCTIVE &&
		command_class != LTFS_CMD_UNKNOWN)
		return false;
	if (sense_valid && sg_sense_proves_known_outcome(sense_key))
		return false;
	if (command_class == LTFS_CMD_WRITE && req->dxfer_len > 0 &&
		req->resid >= 0 &&
		(unsigned int)req->resid == req->dxfer_len)
		return false;
	return true;
}

static bool sg_sense_proves_sequential_outcome(const sg_io_hdr_t *req,
	int ret, uint8_t sense_key, const unsigned char *sense_buffer)
{
	if (req->cmdp[0] == READ) {
		if (ret == -EDEV_FILEMARK_DETECTED ||
			ret == -EDEV_CLEANING_REQUIRED ||
			ret == -EDEV_EOD_DETECTED ||
			ret == -EDEV_RECORD_NOT_FOUND ||
			ret == -EDEV_EOD_NOT_FOUND)
			return true;
		if (sense_key == 0x00 && ret == -EDEV_NO_SENSE &&
			(sense_buffer[2] & (SK_FM_SET | SK_ILI_SET)))
			return true;
	} else if (req->cmdp[0] == WRITE || req->cmdp[0] == WRITE_FILEMARKS6) {
		if (ret == -EDEV_EARLY_WARNING ||
			ret == -EDEV_PROG_EARLY_WARNING ||
			ret == -EDEV_CLEANING_REQUIRED)
			return true;
	}
	return false;
}

static const char *sg_ambiguous_message(
	enum ltfs_retry_command_class command_class)
{
	if (command_class == LTFS_CMD_RELATIVE_POSITIONING)
		return "scsi.stop.ambiguous_position";
	if (command_class == LTFS_CMD_STATE_CHANGING ||
		command_class == LTFS_CMD_DESTRUCTIVE ||
		command_class == LTFS_CMD_UNKNOWN)
		return "scsi.stop.ambiguous_state_change";
	return "scsi.stop.ambiguous_write";
}

static uint64_t sg_retry_deadline_ms(const sg_io_hdr_t *req)
{
	if (req->timeout > 0 && req->timeout < LTFS_SG_RETRY_DEADLINE_MS)
		return req->timeout;
	return LTFS_SG_RETRY_DEADLINE_MS;
}

static void sg_record_retry_decision(struct sg_tape *device,
	const struct ltfs_retry_decision *decision, uint8_t operation)
{
	int result = -EDEV_RETRY;
	if (decision->action == LTFS_RETRY_SUCCESS) {
		(void)__atomic_add_fetch(&device->recovered_error_count, 1,
			__ATOMIC_RELAXED);
		result = DEVICE_GOOD;
	}
	ltfsmsg(LTFS_INFO, 30205I, decision->message_code, operation, result);
	if (device->retry_observer)
		device->retry_observer(decision->message_code, decision->action,
			operation, device->retry_observer_opaque);
}

static int sg_apply_retry_decision(struct sg_tape *device,
	const struct ltfs_retry_decision *decision, unsigned int *attempt,
	bool *unit_attention_seen, uint64_t started_ms, uint64_t deadline_ms,
	uint8_t operation, char **msg)
{
	uint64_t now_ms;
	uint64_t elapsed_ms;
	uint64_t remaining_ms;
	unsigned int delay_ms;
	sg_record_retry_decision(device, decision, operation);
	if (decision->action == LTFS_STOP_AMBIGUOUS) {
		*msg = (char *)decision->message_code;
		return -EDEV_WRITE_AMBIGUOUS;
	}
	if (decision->action != LTFS_RETRY_AFTER_MS)
		return 0;
	if (decision->refresh_identity) {
		int refresh_result;
		if (!device->refresh_identity) {
			*msg = "scsi.fail.identity_refresh_unavailable";
			return -EDEV_IDENTITY_REFRESH_MISSING;
		}
		refresh_result = device->refresh_identity(device,
			device->refresh_identity_opaque);
		if (refresh_result == -EDEV_IDENTITY_MISMATCH) {
			*msg = "scsi.fail.identity_mismatch";
			return refresh_result;
		}
		if (refresh_result < 0) {
			*msg = "scsi.fail.identity_refresh_error";
			return -EDEV_IDENTITY_REFRESH_ERROR;
		}
		*unit_attention_seen = true;
	}
	now_ms = sg_monotonic_ms();
	if (now_ms < started_ms || now_ms - started_ms >= deadline_ms) {
		*msg = "scsi.fail.retry_deadline";
		return -EDEV_TIMEOUT;
	}
	elapsed_ms = now_ms - started_ms;
	remaining_ms = deadline_ms - elapsed_ms;
	delay_ms = decision->delay_ms;
	if ((uint64_t)delay_ms > remaining_ms)
		delay_ms = (unsigned int)remaining_ms;
	if (sg_sleep_ms(delay_ms) < 0)
		return 0;
	now_ms = sg_monotonic_ms();
	if (now_ms < started_ms || now_ms - started_ms >= deadline_ms) {
		*msg = "scsi.fail.retry_deadline";
		return -EDEV_TIMEOUT;
	}
	++(*attempt);
	return 1;
}

static bool is_expected_error(struct sg_tape *device, uint8_t *cdb, int32_t rc )
{
	int cmd = (cdb[0]&0xFF);
	uint64_t destination;
	uint64_t cdb_dest[8];
	int i;

	switch (cmd) {
		case TEST_UNIT_READY:
			if (rc == -EDEV_NEED_INITIALIZE || rc == -EDEV_CONFIGURE_CHANGED)
				return true;
			break;
		case READ:
			if (rc == -EDEV_FILEMARK_DETECTED || rc == -EDEV_NO_SENSE || rc == -EDEV_CLEANING_REQUIRED)
				return true;
			if ((rc == -EDEV_CRYPTO_ERROR || rc == -EDEV_KEY_REQUIRED) && !device->is_data_key_set)
				return true;
			break;
		case WRITE:
			if (rc == -EDEV_EARLY_WARNING || rc == -EDEV_PROG_EARLY_WARNING || rc == -EDEV_CLEANING_REQUIRED)
				return true;
			break;
		case WRITE_FILEMARKS6:
			if (rc == -EDEV_EARLY_WARNING || rc == -EDEV_PROG_EARLY_WARNING || rc == -EDEV_CLEANING_REQUIRED)
				return true;
			break;
		case LOAD_UNLOAD:
			if ((cdb[4] & 0x01) == 0) // Unload
				if (rc == -EDEV_CLEANING_REQUIRED)
					return true;
			break;
		case MODE_SELECT10:
			if (rc == -EDEV_MODE_PARAMETER_ROUNDED)
				return true;
			break;
		case LOCATE16:
			for (i=0; i<8; i++)
				cdb_dest[i] = (uint64_t)cdb[i+4] & 0xff;

			destination = (cdb_dest[0] << 56) + (cdb_dest[1] << 48)
			+ (cdb_dest[2] << 40) + (cdb_dest[3] << 32)
			+ (cdb_dest[4] << 24) + (cdb_dest[5] << 16)
			+ (cdb_dest[6] << 8) + cdb_dest[7];

			if (destination == TAPE_BLOCK_MAX && rc == -EDEV_EOD_DETECTED)
				return true;
			break;
	}

	return false;
}

/* Global functions */
#define HOST_OK          (0x00)
#define HOST_NO_CONNECT  (0x01)
#define HOST_BUS_BUSY    (0x02)
#define HOST_TIME_OUT    (0x03)
#define HOST_BAD_TARGET  (0x04)
#define HOST_ABORT       (0x05)
#define HOST_PARITY      (0x06)
#define HOST_ERROR       (0x07)
#define HOST_RESET       (0x08)
#define HOST_BAD_INTR    (0x09)
#define HOST_PASSTHROUGH (0x0a)
#define HOST_SOFT_ERROR  (0x0b)
#define HOST_IMM_RETRY   (0x0c)
#define HOST_REQUEUE     (0x0d)
#define HOST_TRANS_DISR  (0x0e)
#define HOST_TRANS_FAIL  (0x0f)
#define HOST_TARGET_FAIL (0x10)
#define HOST_NEXUS_FAIL  (0x11)

#define DRIVER_OK        (0x00)
#define DRIVER_BUSY      (0x01)
#define DRIVER_SOFT      (0x02)
#define DRIVER_MEDIA     (0x03)
#define DRIVER_ERROR     (0x04)
#define DRIVER_INVALID   (0x05)
#define DRIVER_TIMEOUT   (0x06)
#define DRIVER_HARD      (0x07)
#define DRIVER_SENSE     (0x08)

#define NO_SUGGESTION    (0x00)
#define SUGGEST_RETRY    (0x10)
#define SUGGEST_ABORT    (0x20)
#define SUGGEST_REMAP    (0x30)
#define SUGGEST_DIE      (0x40)
#define SUGGEST_SENSE    (0x80)

int sg_issue_cdb_command(struct sg_tape *device, sg_io_hdr_t *req, char **msg)
{
	int ret = -1;
	uint32_t sense = 0;
	unsigned short d_suggest = 0, d_status;
	unsigned char masked_status = 0;
	unsigned int attempt = 0;
	bool unit_attention_seen = false;
	uint64_t started_ms;
	uint64_t deadline_ms;
	enum ltfs_retry_command_class command_class;

	CHECK_ARG_NULL(device, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(req, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(msg, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(req->cmdp, -LTFS_NULL_ARG);

	if (device->fd < 0)
		return -EDEV_NO_CONNECTION;
	started_ms = sg_monotonic_ms();
	deadline_ms = sg_retry_deadline_ms(req);
	command_class = sg_command_class(req);

start:
	ret = -1;
	d_suggest = 0;
	masked_status = 0;
	ret = LTFS_SG_IOCTL(device->fd, SG_IO, req);
	if (ret < 0) {
		struct ltfs_retry_input input = {
			command_class, 0x0b, 0x00, 0x00, attempt,
			sg_monotonic_ms() - started_ms, deadline_ms,
			sg_operation_may_be_ambiguous(req, command_class,
				false, 0),
			unit_attention_seen,
			false,
		};
		struct ltfs_retry_decision decision = ltfs_retry_classify(&input);
		ltfsmsg(LTFS_INFO, 30200I, *req->cmdp, errno);
		if (decision.action == LTFS_STOP_AMBIGUOUS) {
			*msg = (char *)decision.message_code;
			return -EDEV_WRITE_AMBIGUOUS;
		}
		if (errno == ENODEV) {
			if (msg) *msg = "No device found";
			return -EDEV_CONNECTION_LOST;
		} else if (errno == ENOMEM) {
			if (msg) *msg = "ioctl ENOMEM error";
			return -EDEV_BUFFER_ALLOCATE_ERROR;
		} else {
			if (msg) *msg = "ioctl error";
			return -EDEV_INTERNAL_ERROR;
		}
	}

	if (req->host_status) {
		bool retry_requested = false;
		switch (req->host_status) {
			case HOST_NO_CONNECT:
				if (msg) *msg = "Couldn't connect before timeout period";
				ret = -EDEV_CONNECTION_LOST;
				break;
			case HOST_BUS_BUSY:
				if (msg) *msg = "Bus stayed busy through timeout period";
				ret = -EDEV_DEVICE_BUSY;
				break;
			case HOST_TIME_OUT:
				if (msg) *msg = "Command TIMEOUT";
				ret = -EDEV_TIMEOUT;
				break;
			case HOST_BAD_TARGET:
				if (msg) *msg = "Bad target, device not responding?";
				ret = -EDEV_CONNECTION_LOST;
				break;
			case HOST_ABORT:
				if (msg) *msg = "Abort";
				ret = -EDEV_ABORTED_COMMAND;
				break;
			case HOST_PARITY:
				if (msg) *msg = "Parity error";
				ret = -EDEV_HOST_ERROR;
				break;
			case HOST_ERROR:
				if (msg) *msg = "Internal error detected in the host adapter";
				ret = -EDEV_HOST_ERROR;
				break;
			case HOST_RESET:
				if (msg) *msg = "The SCSI bus (or this device) has been reset";
				ret = -EDEV_CONNECTION_LOST;
				break;
			case HOST_BAD_INTR:
				if (msg) *msg = "Unexpected interrupt";
				ret = -EDEV_HOST_ERROR;
				break;
			case HOST_PASSTHROUGH:
				if (msg) *msg = "Force command past mid-layer";
				ret = -EDEV_HOST_ERROR;
				break;
			case HOST_SOFT_ERROR:
				if (msg) *msg = "The low level driver wants a retry";
				ret = -EDEV_HOST_ERROR;
				retry_requested = true;
				break;
			case HOST_IMM_RETRY:
			case HOST_REQUEUE:
				if (msg) *msg = "The low level driver requests retry";
				ret = -EDEV_HOST_ERROR;
				retry_requested = true;
				break;
			case HOST_TRANS_DISR:
				if (msg) *msg = "Disrupted transport failure";
				ret = -EDEV_CONNECTION_LOST;
				break;
			case HOST_TRANS_FAIL:
				if (msg) *msg = "Transport failure";
				ret = -EDEV_CONNECTION_LOST;
				break;
			case HOST_TARGET_FAIL:
				if (msg) *msg = "Target failure";
				ret = -EDEV_CONNECTION_LOST;
				break;
			case HOST_NEXUS_FAIL:
				/* See https://fossies.org/linux/sdparm/lib/sg_pt_linux.c */
				if (msg) *msg = "SCSI nexus failure (reservation conflict)";
				ret = -EDEV_RESERVATION_CONFLICT;
				break;
			default:
				ltfsmsg(LTFS_INFO, 30244I, req->host_status, req->driver_status);
				if (msg) *msg = "Unexpected host status";
				ret = -EDEV_HOST_ERROR;
				break;
		}

		if (retry_requested) {
			struct ltfs_retry_input input = {
				command_class, 0x0b, 0x00, 0x00, attempt,
				sg_monotonic_ms() - started_ms, deadline_ms,
				sg_operation_may_be_ambiguous(req, command_class,
					false, 0),
				unit_attention_seen,
				false,
			};
			struct ltfs_retry_decision decision = ltfs_retry_classify(&input);
			int retry_result = sg_apply_retry_decision(device, &decision,
				&attempt, &unit_attention_seen, started_ms, deadline_ms,
				req->cmdp[0], msg);
			if (retry_result > 0)
				goto start;
			if (retry_result < 0)
				return retry_result;
		}
		if (ret && sg_operation_may_be_ambiguous(req, command_class,
			false, 0)) {
			*msg = (char *)sg_ambiguous_message(command_class);
			return -EDEV_WRITE_AMBIGUOUS;
		}
		if (ret)
			return ret;
	}

	if (req->driver_status) {
		bool retry_requested = false;
		d_suggest = req->driver_status & 0xF0;
		d_status  = req->driver_status & 0x0F;

		switch (d_status) {
			case DRIVER_OK:
				/* Do nothing */
				break;
			case DRIVER_BUSY:
				if (msg) *msg = "Busy on the driver";
				ret = -EDEV_DEVICE_BUSY;
				break;
			case DRIVER_TIMEOUT:
				if (msg) *msg = "Timeout on the driver";
				ret = -EDEV_TIMEOUT;
				break;
			case DRIVER_SENSE:
				masked_status = SCSI_CHECK_CONDITION;
				break;
			case DRIVER_SOFT:
			case DRIVER_MEDIA:
			case DRIVER_ERROR:
			case DRIVER_INVALID:
			case DRIVER_HARD:
			default:
				ltfsmsg(LTFS_INFO, 30244I, req->host_status, req->driver_status);
				if (msg) *msg = "Busy on the driver";
				ret = -EDEV_DRIVER_ERROR;
				break;
		}

		if (d_suggest == SUGGEST_SENSE) {
			masked_status = SCSI_CHECK_CONDITION;
			ret = 0;
		} else if (masked_status != SCSI_CHECK_CONDITION) {
			switch (d_suggest) {
				case NO_SUGGESTION:
					/* Do nothing */
					break;
				case SUGGEST_RETRY:
					retry_requested = true;
					if (!ret)
						ret = -EDEV_DRIVER_ERROR;
					break;
				case SUGGEST_ABORT:
				case SUGGEST_REMAP:
				case SUGGEST_DIE:
				default:
					if (!ret)
						ret = -EDEV_DRIVER_ERROR;
					break;
			}
		}

		if (retry_requested) {
			struct ltfs_retry_input input = {
				command_class, 0x0b, 0x00, 0x00, attempt,
				sg_monotonic_ms() - started_ms, deadline_ms,
				sg_operation_may_be_ambiguous(req, command_class,
					false, 0),
				unit_attention_seen,
				false,
			};
			struct ltfs_retry_decision decision = ltfs_retry_classify(&input);
			int retry_result = sg_apply_retry_decision(device, &decision,
				&attempt, &unit_attention_seen, started_ms, deadline_ms,
				req->cmdp[0], msg);
			if (retry_result > 0)
				goto start;
			if (retry_result < 0)
				return retry_result;
		}
		if (ret && sg_operation_may_be_ambiguous(req, command_class,
			false, 0)) {
			*msg = (char *)sg_ambiguous_message(command_class);
			return -EDEV_WRITE_AMBIGUOUS;
		}
		if (ret)
			return ret;
	}

	if (masked_status != SCSI_CHECK_CONDITION)
		masked_status = req->masked_status;

	switch (masked_status) {
		case SCSI_GOOD:
			ret = DEVICE_GOOD;
			break;
		case SCSI_CHECK_CONDITION:
			if (req->sb_len_wr >= 14) {
				struct ltfs_retry_input input;
				struct ltfs_retry_decision decision;
				int retry_result;
				bool completion_may_be_ambiguous;
				unsigned char *sense_buffer = req->sbp;
				uint8_t sense_key = sense_buffer[2] & 0x0f;
				uint8_t asc = sense_buffer[12];
				uint8_t ascq = sense_buffer[13];
				ret = sg_sense2errno(req, &sense, msg);
				ltfsmsg(LTFS_DEBUG, 30201D, sense, *msg);
				completion_may_be_ambiguous =
					sg_operation_may_be_ambiguous(req, command_class,
						true, sense_key);
				if (sg_sense_proves_sequential_outcome(req, ret,
					sense_key, sense_buffer))
					completion_may_be_ambiguous = false;
				input = (struct ltfs_retry_input){
					command_class, sense_key, asc, ascq, attempt,
					sg_monotonic_ms() - started_ms, deadline_ms,
					completion_may_be_ambiguous,
					unit_attention_seen,
					ret == DEVICE_GOOD,
				};
				decision = ltfs_retry_classify(&input);
				if (decision.action == LTFS_RETRY_SUCCESS) {
					sg_record_retry_decision(device, &decision,
						req->cmdp[0]);
					ret = DEVICE_GOOD;
					break;
				}
				retry_result = sg_apply_retry_decision(device, &decision,
					&attempt, &unit_attention_seen, started_ms, deadline_ms,
					req->cmdp[0], msg);
				if (retry_result > 0)
					goto start;
				if (retry_result < 0)
					return retry_result;
			} else {
				ret = -EDEV_NO_SENSE;
				ltfsmsg(LTFS_DEBUG, 30202D, "nosense");
				if (sg_operation_may_be_ambiguous(req, command_class,
					false, 0)) {
					*msg = (char *)sg_ambiguous_message(command_class);
					return -EDEV_WRITE_AMBIGUOUS;
				}
			}
			break;
		case SCSI_BUSY:
			ltfsmsg(LTFS_DEBUG, 30202D, "busy");
			ret = -EDEV_DEVICE_BUSY;
			if (msg) *msg = "Drive busy";
			break;
		case SCSI_RESERVATION_CONFLICT:
			ltfsmsg(LTFS_DEBUG, 30202D, "reservation conflict");
			ret = -EDEV_RESERVATION_CONFLICT;
			if (msg) *msg = "Drive reservation conflict";
			break;
		default:
			ltfsmsg(LTFS_INFO, 30203I, req->status, req->masked_status);
			if (sg_operation_may_be_ambiguous(req, command_class,
				false, 0)) {
				*msg = (char *)sg_ambiguous_message(command_class);
				return -EDEV_WRITE_AMBIGUOUS;
			}
			ret = -EDEV_TARGET_ERROR;
			if (msg) *msg = "CDB command returned unexpected status";
			break;
	}

	if (ret != DEVICE_GOOD) {
		if (is_expected_error(device, req->cmdp, ret)) {
			ltfsmsg(LTFS_DEBUG, 30204D, (char *)req->usr_ptr, req->cmdp[0], ret);
		} else {
			ltfsmsg(LTFS_INFO, 30205I, (char *)req->usr_ptr, req->cmdp[0], ret);
		}
	}

	return ret;
}

static int _inquiry_low(struct sg_tape *device, uint8_t page,
	unsigned char *buf, size_t size, size_t *received_size)
{
	int ret = -EDEV_UNKNOWN;

	sg_io_hdr_t req;
	unsigned char cdb[CDB6_LEN];
	unsigned char sense[MAXSENSE];
	char cmd_desc[COMMAND_DESCRIPTION_LENGTH] = "INQUIRY LOW";
	char *msg;

	// Zero out the CDB and the result buffer
	ret = init_sg_io_header(&req);
	if (ret < 0)
		return ret;

	memset(cdb, 0, sizeof(cdb));
	memset(sense, 0, sizeof(sense));
	memset(buf, 0, size);

	/* Build CDB */
	cdb[0] = INQUIRY;
	if(page)
		cdb[1] = 0x01;
	cdb[2] = page;
	cdb[3] = (unsigned char)((size >> 8) & 0xffU);
	cdb[4] = (unsigned char)(size & 0xffU);

	/* Build request */
	req.dxfer_direction = SCSI_FROM_TARGET_TO_INITIATOR;
	req.cmd_len         = sizeof(cdb);
	req.mx_sb_len       = sizeof(sense);
	req.dxfer_len       = size;
	req.dxferp          = buf;
	req.cmdp            = cdb;
	req.sbp             = sense;
	req.timeout         = SGConversion(10);
	req.usr_ptr         = (void *)cmd_desc;

	ret = sg_issue_cdb_command(device, &req, &msg);
	if (ret >= 0 && received_size) {
		if (req.resid < 0 || (size_t)req.resid > size)
			return -EDEV_LENGTH_MISMATCH;
		*received_size = size - (size_t)req.resid;
	}

	return ret;
}

int sg_get_drive_identifier(struct sg_tape *device, scsi_device_identifier *id_data)
{
	int ret;
	size_t serial_length;
	size_t received_size;
	unsigned char inquiry_buf[MAX_INQ_LEN];

	CHECK_ARG_NULL(id_data, -LTFS_NULL_ARG);

	ret = _inquiry_low(device, 0, inquiry_buf, MAX_INQ_LEN,
		&received_size);
	if( ret < 0 ) {
		ltfsmsg(LTFS_INFO, 30206I, ret);
		return ret;
	}
	if (received_size < 36)
		return -EDEV_LENGTH_MISMATCH;

	memset(id_data, 0, sizeof(scsi_device_identifier));

	if ((inquiry_buf[0] & PERIPHERAL_MASK) != SEQUENTIAL_DEVICE) {
		/* TODO: Need a debug message */
		return -EDEV_DEVICE_UNSUPPORTABLE;
	}

	strncpy(id_data->vendor_id,   (char*)(&(inquiry_buf[8])),  VENDOR_ID_LENGTH);
	strncpy(id_data->product_id,  (char*)(&(inquiry_buf[16])), PRODUCT_ID_LENGTH);
	strncpy(id_data->product_rev, (char*)(&(inquiry_buf[32])), PRODUCT_REV_LENGTH);

	ret = _inquiry_low(device, 0x80, inquiry_buf, MAX_INQ_LEN,
		&received_size);
	if( ret < 0 ) {
		ltfsmsg(LTFS_INFO, 30206I, ret);
		return ret;
	}
	if (received_size < 4)
		return -EDEV_LENGTH_MISMATCH;

	serial_length = ((size_t)inquiry_buf[2] << 8) | inquiry_buf[3];
	if (serial_length > received_size - 4)
		return -EDEV_LENGTH_MISMATCH;
	if (serial_length > UNIT_SERIAL_LENGTH)
		serial_length = UNIT_SERIAL_LENGTH;
	memcpy(id_data->unit_serial, &inquiry_buf[4], serial_length);
	id_data->unit_serial[serial_length] = '\0';

	return 0;
}

int sg_bind_drive_identity(struct sg_tape *device,
	const scsi_device_identifier *id_data)
{
	CHECK_ARG_NULL(device, -LTFS_NULL_ARG);
	CHECK_ARG_NULL(id_data, -LTFS_NULL_ARG);
	if (!id_data->unit_serial[0])
		return -EDEV_INVALID_ARG;
	if (device->identity_serial[0] &&
		strcmp(device->identity_serial, id_data->unit_serial) != 0)
		return -EDEV_IDENTITY_MISMATCH;
	if (!device->identity_serial[0])
		strncpy(device->identity_serial, id_data->unit_serial,
			sizeof(device->identity_serial) - 1);
	device->refresh_identity = sg_refresh_drive_identity;
	device->refresh_identity_opaque = NULL;
	return 0;
}

void sg_set_retry_observer(struct sg_tape *device,
	sg_retry_observer_fn observer, void *opaque)
{
	if (!device)
		return;
	device->retry_observer = observer;
	device->retry_observer_opaque = opaque;
}

uint64_t sg_get_recovered_error_count(const struct sg_tape *device)
{
	if (!device)
		return 0;
	return __atomic_load_n(&device->recovered_error_count, __ATOMIC_RELAXED);
}

static int sg_refresh_drive_identity(struct sg_tape *device, void *opaque)
{
	scsi_device_identifier refreshed;
	sg_identity_refresh_fn saved_refresh;
	int ret;
	(void)opaque;
	if (!device || !device->identity_serial[0])
		return -EDEV_INVALID_ARG;
	saved_refresh = device->refresh_identity;
	device->refresh_identity = NULL;
	ret = sg_get_drive_identifier(device, &refreshed);
	device->refresh_identity = saved_refresh;
	if (ret == 0 && strcmp(device->identity_serial,
		refreshed.unit_serial) != 0)
		ret = -EDEV_IDENTITY_MISMATCH;
	return ret;
}
