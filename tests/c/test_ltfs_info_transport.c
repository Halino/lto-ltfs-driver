/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise the real read-only SG transport with the kernel ioctl boundary
 * replaced. No device is opened and no hardware command can leave this test. */
#include <stdarg.h>
#define ioctl test_info_ioctl
#define main unused_info_main
#include "../../src/utils/ltfs_info.c"
#undef main
#undef ioctl
#include "test.h"

static struct {
	int calls;
	bool truncated, bad_residual, sense_error, wrong_serial, page17;
	int opens, closes, coherencies, truncate_coherency;
	bool capacity_read, change_post_mam, change_post_drive;
} wire;

int test_info_ioctl(int fd, unsigned long request, ...)
{
	va_list arguments;
	sg_io_hdr_t *io;
	unsigned char *data;
	CHECK_INT_EQ(fd, 88);
	CHECK_INT_EQ(request, SG_IO);
	va_start(arguments, request);
	io = va_arg(arguments, sg_io_hdr_t *);
	va_end(arguments);
	++wire.calls;
	CHECK_TRUE(ltfs_info_opcode_allowed(io->cmdp[0]));
	if (io->cmdp[0] == SCSI_TEST_UNIT_READY) {
		CHECK_INT_EQ(io->dxfer_direction, SG_DXFER_NONE);
		CHECK_INT_EQ(io->dxfer_len, 0);
		return 0;
	}
	CHECK_INT_EQ(io->dxfer_direction, SG_DXFER_FROM_DEV);
	data = io->dxferp;
	if (wire.sense_error) {
		io->info = SG_INFO_CHECK;
		io->status = 2;
		io->sb_len_wr = 14;
		io->sbp[0] = 0x70;
		io->sbp[2] = 4;
		return 0;
	}
	if (io->cmdp[0] == SCSI_INQUIRY && io->cmdp[1] == 0) {
		CHECK_INT_EQ(io->cmd_len, 6);
		CHECK_INT_EQ(io->cmdp[4], 96);
		data[0] = 1;
		data[4] = 91;
		memcpy(data + 8, "HP      ", 8);
		memcpy(data + 16, wire.page17 ? "Ultrium 7-SCSI   " : "Ultrium 6-SCSI   ", 16);
	} else if (io->cmdp[0] == SCSI_INQUIRY && io->cmdp[1] == 1) {
		const char *serial = (wire.wrong_serial ||
			(wire.change_post_drive && wire.capacity_read)) ? "WRONG-SERIAL!" : "DRIVE-TEST-01";
		CHECK_INT_EQ(io->cmdp[2], 0x80);
		data[0] = 1;
		data[1] = 0x80;
		data[3] = 13;
		memcpy(data + 4, serial, 13);
		io->resid = io->dxfer_len - 17;
	} else if (io->cmdp[0] == SCSI_READ_ATTRIBUTE) {
		unsigned char attribute[300] = {0};
		uint16_t attr = ((uint16_t)io->cmdp[8] << 8) | io->cmdp[9];
		const char *text = NULL;
		size_t size;
		CHECK_INT_EQ(io->cmd_len, 16);
		CHECK_INT_EQ(io->cmdp[7], 0);
		attribute[4] = io->cmdp[8];
		attribute[5] = io->cmdp[9];
		if (attr == LTFS_INFO_COHERENCY) {
			unsigned char *descriptor = attribute + 4;
			++wire.coherencies;
			descriptor[4] = 70;
			descriptor[5] = 8;
			descriptor[21] = 42;
			descriptor[31] = 43;
			memcpy(descriptor + 32, "LTFS", 5);
			memcpy(descriptor + 37, "11111111-2222-3333-4444-555555555555", 36);
			descriptor[74] = 1;
			size = 79;
		} else {
			switch (attr) {
			case LTFS_INFO_APP_NAME: text = "LTFS"; break;
			case LTFS_INFO_APP_VERSION: text = "TEST"; break;
			case LTFS_INFO_VOLUME_LABEL: text = "TEST VOLUME"; break;
			case LTFS_INFO_BARCODE: text = "TEST01"; break;
			case TC_MAM_MEDIUM_SERIAL_NUMBER:
				text = wire.change_post_mam && wire.capacity_read ? "CHANGED" : "SERIAL-TEST-01";
				break;
			default: CHECK_TRUE(false);
			}
			attribute[6] = 1;
			attribute[8] = strlen(text);
			memcpy(attribute + 9, text, strlen(text));
			size = 9 + strlen(text);
		}
		attribute[3] = size - 4;
		memcpy(data, attribute, size);
		io->resid = io->dxfer_len - size;
		if (attr == LTFS_INFO_COHERENCY && wire.truncate_coherency == wire.coherencies)
			io->resid += 2; /* Header declares full identity; tail not transferred. */
	} else if (io->cmdp[0] == SCSI_LOG_SENSE) {
		const unsigned char page31[] = {
			0x31,0,0,32,
			0,1,0,4, 0,0,0,10,
			0,2,0,4, 0,0x1e,0x84,0x80,
			0,3,0,4, 0,0,0,30,
			0,4,0,4, 0,0x24,0x9f,0,
		};
		const unsigned char page17[] = {
			0x17,0,0,40,
			2,2,0,16, 7,0,0,0,0,0,0,100, 7,0,0,1,0,0x26,0x25,0xa0,
			2,4,0,16, 7,0,0,0,0,0,0,10, 7,0,0,1,0,0x1e,0x84,0x80,
		};
		CHECK_INT_EQ(io->cmd_len, 10);
		wire.capacity_read = true;
		CHECK_INT_EQ(io->cmdp[2], wire.page17 ? 0x57 : 0x71);
		CHECK_INT_EQ(io->cmdp[3], 0);
		CHECK_INT_EQ(io->cmdp[1], 0);
		CHECK_INT_EQ(io->cmdp[5], 0);
		CHECK_INT_EQ(io->cmdp[6], 0);
		CHECK_INT_EQ(io->cmdp[7], 4);
		CHECK_INT_EQ(io->cmdp[8], 0);
		memcpy(data, wire.page17 ? page17 : page31, wire.page17 ? sizeof(page17) : sizeof(page31));
		io->resid = io->dxfer_len - (wire.page17 ? sizeof(page17) : sizeof(page31));
	} else {
		CHECK_TRUE(false);
	}
	if (wire.truncated) ++io->resid;
	if (wire.bad_residual) io->resid = -1;
	return 0;
}

static int session_open(void *context, const char *path)
{
	struct linux_sg_backend *backend = context;
	CHECK_TRUE(!strcmp(path, "/fixture/sg"));
	CHECK_INT_EQ(backend->fd, -1);
	backend->fd = 88;
	++wire.opens;
	return 0;
}

static int session_close(void *context)
{
	struct linux_sg_backend *backend = context;
	CHECK_INT_EQ(backend->fd, 88);
	backend->fd = -1;
	++wire.closes;
	return 0;
}

static int test_full_wire_session(void)
{
	const struct ltfs_info_expected_medium expected = {
		.drive_serial = "DRIVE-TEST-01", .medium_serial = "SERIAL-TEST-01",
		.volume_label = "TEST VOLUME",
		.volume_uuid = "11111111-2222-3333-4444-555555555555", .index_generation = 42,
	};
	struct ltfs_info_identity identity = {0};
	strcpy(identity.drive_serial, "DRIVE-TEST-01");
	for (int scenario = 0; scenario < 5; ++scenario) {
		struct linux_sg_backend context = {
			.fd = -1, .capacity_diagnostic = true, .expected_serial = "DRIVE-TEST-01",
		};
		struct ltfs_info_backend backend = {
			.read_only_guaranteed = true, .context = &context,
			.open = session_open, .close = session_close,
			.inquiry = linux_inquiry, .test_unit_ready = linux_ready,
			.read_text_attribute = linux_text, .read_coherency = linux_coherency,
			.remaining_capacity = linux_capacity,
		};
		struct ltfs_info_capacity_report report;
		memset(&wire, 0, sizeof(wire));
		wire.truncate_coherency = scenario == 1 ? 1 : scenario == 2 ? 2 : 0;
		wire.change_post_mam = scenario == 3;
		wire.change_post_drive = scenario == 4;
		int result = ltfs_info_collect_capacity("/fixture/sg", &identity, &backend,
			&expected, &report);
		CHECK_INT_EQ(result, scenario == 0 ? 0 : scenario == 3 ?
			LTFS_INFO_IDENTITY_MISMATCH : LTFS_INFO_READ_FAILURE);
		CHECK_INT_EQ(wire.opens, 1);
		CHECK_INT_EQ(wire.closes, 1);
		CHECK_INT_EQ(context.fd, -1);
		CHECK_INT_EQ(report.identity_verified_before_after, scenario == 0);
		CHECK_INT_EQ(report.record.capacity_valid, scenario == 0);
		if (!scenario) {
			CHECK_INT_EQ(wire.coherencies, 2);
			CHECK_INT_EQ(report.record.capacity.remaining_partition1_mib, 2000000);
		}
	}
	return 0;
}

int main(void)
{
	struct linux_sg_backend backend = {
		.fd = 88, .capacity_diagnostic = true, .expected_serial = "DRIVE-TEST-01",
	};
	struct ltfs_info_capacity capacity = {0};
	unsigned char forbidden_cdb[6] = {0x15};
	CHECK_INT_EQ(linux_inquiry(&backend), 0);
	CHECK_INT_EQ(linux_capacity(&backend, &capacity), 0);
	CHECK_INT_EQ(wire.calls, 3);
	CHECK_INT_EQ(capacity.log_page, 0x31);
	CHECK_INT_EQ(capacity.remaining_partition1_mib, 2000000);
	CHECK_INT_EQ(capacity.maximum_partition1_mib, 2400000);
	CHECK_INT_EQ(capacity.capacity_offset, 0);
	CHECK_INT_EQ(linux_sg_command(&backend, forbidden_cdb, sizeof(forbidden_cdb), NULL, 0),
		LTFS_INFO_BACKEND_READ_ERROR);
	CHECK_INT_EQ(wire.calls, 3);
	wire.truncated = true;
	CHECK_INT_EQ(linux_capacity(&backend, &capacity), LTFS_INFO_BACKEND_READ_ERROR);
	wire.truncated = false;
	wire.bad_residual = true;
	CHECK_INT_EQ(linux_capacity(&backend, &capacity), LTFS_INFO_BACKEND_READ_ERROR);
	wire.bad_residual = false;
	wire.sense_error = true;
	wire.calls = 0;
	CHECK_INT_EQ(linux_capacity(&backend, &capacity), LTFS_INFO_BACKEND_READ_ERROR);
	CHECK_INT_EQ(wire.calls, 1); /* No recovery command, dump or retry. */
	wire.sense_error = false;
	wire.wrong_serial = true;
	CHECK_INT_EQ(linux_inquiry(&backend), LTFS_INFO_BACKEND_READ_ERROR);
	wire.wrong_serial = false;
	wire.page17 = true;
	/* Changed inquiry family in the same session is rejected. */
	CHECK_INT_EQ(linux_inquiry(&backend), LTFS_INFO_BACKEND_READ_ERROR);
	backend.capacity_page = 0;
	CHECK_INT_EQ(linux_inquiry(&backend), 0);
	CHECK_INT_EQ(linux_capacity(&backend, &capacity), 0);
	CHECK_INT_EQ(capacity.log_page, 0x17);
	CHECK_INT_EQ(capacity.remaining_partition1_mib, 1907348);
	CHECK_INT_EQ(capacity.maximum_partition1_mib, 2384185);
	{
		char application[256];
		CHECK_INT_EQ(linux_text(&backend, 0x0801, application, sizeof(application)), 0);
		CHECK_TRUE(!strcmp(application, "LTFS"));
		wire.truncated = true;
		CHECK_INT_EQ(linux_text(&backend, 0x0801, application, sizeof(application)),
			LTFS_INFO_BACKEND_READ_ERROR);
	}
	CHECK_INT_EQ(test_full_wire_session(), 0);
	return 0;
}
