/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "ltfs_info.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_TEST_UNIT_READY 0x00
#define OP_FORMAT_MEDIUM 0x04
#define OP_WRITE 0x0a
#define OP_WRITE_FILEMARKS 0x10
#define OP_ERASE 0x19
#define OP_LOAD_UNLOAD 0x1b
#define OP_MODE_SELECT6 0x15
#define OP_MODE_SELECT10 0x55
#define OP_INQUIRY 0x12
#define OP_LOG_SENSE 0x4d
#define OP_WRITE_BUFFER 0x3b
#define OP_READ_ATTRIBUTE 0x8c

#define ATTR_APPLICATION_NAME 0x0801
#define ATTR_APPLICATION_VERSION 0x0802
#define ATTR_VOLUME_LABEL 0x0803
#define ATTR_BARCODE 0x0806
#define ATTR_VOLUME_IDENTIFIER 0x0008
#define ATTR_MEDIUM_SERIAL_NUMBER 0x0401

struct fake_backend {
	int result;
	int fail_call;
	int call_count;
	bool optional_unavailable;
	bool non_ltfs;
	bool app_name_unavailable;
	bool volume_label_unavailable;
	bool barcode_unavailable;
	bool volume_serial_unavailable;
	bool coherency_unavailable;
	bool capacity_unavailable;
	int close_result;
	int open_count, close_count, capacity_count;
	uint16_t changed_attribute;
	bool changed_uuid, changed_generation;
	char barcode[LTFS_INFO_TEXT_MAX];
	char volume_serial[LTFS_INFO_TEXT_MAX];
	bool opened;
	bool closed;
	unsigned char opcodes[32];
	size_t opcode_count;
	uint16_t attributes[32];
	size_t attribute_count;
};

static int fake_stage_result(struct fake_backend *fake)
{
	++fake->call_count;
	return fake->fail_call == fake->call_count ? fake->result : 0;
}

static void record_opcode(struct fake_backend *fake, unsigned char opcode)
{
	if (fake->opcode_count < sizeof(fake->opcodes))
		fake->opcodes[fake->opcode_count++] = opcode;
}

static int fake_open(void *context, const char *path)
{
	struct fake_backend *fake = context;
	int result;
	++fake->open_count;
	CHECK_TRUE(path && !strcmp(path, "/dev/sg-test"));
	result = fake_stage_result(fake);
	if (!result)
		fake->opened = true;
	return result;
}

static int fake_close(void *context)
{
	struct fake_backend *fake = context;
	fake->closed = true;
	++fake->close_count;
	return fake->close_result;
}

static int fake_inquiry(void *context)
{
	struct fake_backend *fake = context;
	record_opcode(fake, OP_INQUIRY);
	return fake_stage_result(fake);
}

static int fake_ready(void *context)
{
	struct fake_backend *fake = context;
	record_opcode(fake, OP_TEST_UNIT_READY);
	return fake_stage_result(fake);
}

static int fake_text(void *context, uint16_t attribute, char *value,
	size_t size)
{
	struct fake_backend *fake = context;
	const char *text = NULL;
	int result;
	record_opcode(fake, OP_READ_ATTRIBUTE);
	if (fake->attribute_count < sizeof(fake->attributes) /
			sizeof(fake->attributes[0]))
		fake->attributes[fake->attribute_count++] = attribute;
	result = fake_stage_result(fake);
	if (result)
		return result;
	if (fake->app_name_unavailable && attribute == ATTR_APPLICATION_NAME)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	if (fake->optional_unavailable && attribute != ATTR_APPLICATION_NAME)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	if (attribute == ATTR_APPLICATION_NAME)
		text = fake->non_ltfs ? "OTHER" : "LTFS";
	else if (attribute == ATTR_APPLICATION_VERSION)
		text = "3.4.2";
	else if (attribute == ATTR_VOLUME_LABEL) {
		if (fake->volume_label_unavailable)
			return LTFS_INFO_BACKEND_UNSUPPORTED;
		text = "TEST VOLUME";
	} else if (attribute == ATTR_BARCODE) {
		if (fake->barcode_unavailable)
			return LTFS_INFO_BACKEND_UNSUPPORTED;
		text = fake->barcode[0] ? fake->barcode : "TEST01";
	} else if (attribute == ATTR_MEDIUM_SERIAL_NUMBER) {
		if (fake->volume_serial_unavailable)
			return LTFS_INFO_BACKEND_UNSUPPORTED;
		text = fake->volume_serial[0] ? fake->volume_serial :
			"SERIAL-TEST-01";
	} else
		return LTFS_INFO_BACKEND_READ_ERROR;
	if (fake->capacity_count && fake->changed_attribute == attribute)
		text = "CHANGED";
	if (strlen(text) >= size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	strcpy(value, text);
	return 0;
}

static int fake_coherency(void *context, char uuid[LTFS_INFO_UUID_SIZE],
	uint64_t *generation)
{
	struct fake_backend *fake = context;
	int result;
	record_opcode(fake, OP_READ_ATTRIBUTE);
	result = fake_stage_result(fake);
	if (result)
		return result;
	if (fake->coherency_unavailable)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	strcpy(uuid, "11111111-2222-3333-4444-555555555555");
	*generation = 42;
	if (fake->capacity_count && fake->changed_uuid)
		strcpy(uuid, "aaaaaaaa-2222-3333-4444-555555555555");
	if (fake->capacity_count && fake->changed_generation)
		++*generation;
	return 0;
}

static int fake_capacity(void *context, struct ltfs_info_capacity *capacity)
{
	struct fake_backend *fake = context;
	int result;
	record_opcode(fake, OP_LOG_SENSE);
	++fake->capacity_count;
	result = fake_stage_result(fake);
	if (result)
		return result;
	if (fake->optional_unavailable || fake->capacity_unavailable)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	capacity->remaining_partition0_mib = 10;
	capacity->log_page = 0x31;
	capacity->capacity_offset = 0;
	capacity->remaining_partition1_mib = 20;
	capacity->maximum_partition0_mib = 30;
	capacity->maximum_partition1_mib = 40;
	return 0;
}

static struct ltfs_info_backend backend_for(struct fake_backend *fake)
{
	struct ltfs_info_backend backend = {
		.read_only_guaranteed = true,
		.context = fake,
		.open = fake_open,
		.close = fake_close,
		.inquiry = fake_inquiry,
		.test_unit_ready = fake_ready,
		.read_text_attribute = fake_text,
		.read_coherency = fake_coherency,
		.remaining_capacity = fake_capacity,
	};
	return backend;
}

static struct ltfs_info_identity identity(void)
{
	struct ltfs_info_identity value = {0};
	strcpy(value.target_sha256,
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	strcpy(value.drive_serial, "DRIVE-TEST-01");
	strcpy(value.drive_wwid, "naa.5000test");
	strcpy(value.scsi_tuple, "7:0:0:0");
	return value;
}

static bool forbidden(unsigned char opcode)
{
	return opcode == OP_WRITE || opcode == OP_WRITE_FILEMARKS ||
		opcode == OP_ERASE || opcode == OP_FORMAT_MEDIUM ||
		opcode == OP_LOAD_UNLOAD || opcode == OP_MODE_SELECT6 ||
		opcode == OP_MODE_SELECT10 || opcode == OP_WRITE_BUFFER;
}

static int assert_no_mutating_opcode(const struct fake_backend *fake)
{
	size_t index;
	for (index = 0; index < fake->opcode_count; ++index) {
		CHECK_TRUE(!forbidden(fake->opcodes[index]));
		CHECK_TRUE(ltfs_info_opcode_allowed(fake->opcodes[index]));
	}
	CHECK_TRUE(ltfs_info_opcode_allowed(OP_TEST_UNIT_READY));
	CHECK_TRUE(ltfs_info_opcode_allowed(OP_INQUIRY));
	CHECK_TRUE(ltfs_info_opcode_allowed(OP_LOG_SENSE));
	CHECK_TRUE(ltfs_info_opcode_allowed(OP_READ_ATTRIBUTE));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_WRITE));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_WRITE_FILEMARKS));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_ERASE));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_FORMAT_MEDIUM));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_LOAD_UNLOAD));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_MODE_SELECT6));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_MODE_SELECT10));
	CHECK_TRUE(!ltfs_info_opcode_allowed(OP_WRITE_BUFFER));
	return 0;
}

static int assert_medium_serial_was_read(const struct fake_backend *fake)
{
	size_t index;
	bool found_medium_serial = false;
	for (index = 0; index < fake->attribute_count; ++index) {
		CHECK_TRUE(fake->attributes[index] != ATTR_VOLUME_IDENTIFIER);
		if (fake->attributes[index] == ATTR_MEDIUM_SERIAL_NUMBER)
			found_medium_serial = true;
	}
	CHECK_TRUE(found_medium_serial);
	return 0;
}

static int test_success(void)
{
	struct fake_backend fake = {0};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;
	FILE *json;
	char output[4096] = {0};
	size_t length;

	CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
		&backend, &record), LTFS_INFO_READY);
	CHECK_TRUE(fake.opened && fake.closed && record.ready);
	CHECK_TRUE(record.mam_application_name_valid);
	CHECK_TRUE(!strcmp(record.mam_application_name, "LTFS"));
	CHECK_TRUE(record.capacity_valid);
	CHECK_INT_EQ(record.capacity.remaining_partition1_mib, 20);
	CHECK_TRUE(record.volume_uuid_valid);
	CHECK_TRUE(!strcmp(record.volume_uuid,
		"11111111-2222-3333-4444-555555555555"));
	CHECK_TRUE(record.index_generation_valid);
	CHECK_INT_EQ(record.index_generation, 42);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
	CHECK_INT_EQ(assert_medium_serial_was_read(&fake), 0);
	CHECK_INT_EQ(ltfs_info_set_config_paths(&record,
		"/dev/tape/by-id/test-drive-nst",
		"/dev/lto-archiver-scsi-test-drive-sg"), 0);

	json = tmpfile();
	CHECK_TRUE(json != NULL);
	CHECK_INT_EQ(ltfs_info_write_json(json, &record), 0);
	rewind(json);
	length = fread(output, 1, sizeof(output) - 1, json);
	CHECK_TRUE(length > 0);
	CHECK_STR_EQ(output,
		"{\"schema\":2,\"media_state\":\"ltfs\","
		"\"tape_by_id\":\"/dev/tape/by-id/test-drive-nst\","
		"\"scsi_by_id\":\"/dev/lto-archiver-scsi-test-drive-sg\","
		"\"drive_serial\":\"DRIVE-TEST-01\","
		"\"mam_barcode\":\"TEST01\","
		"\"mam_volume_serial\":\"SERIAL-TEST-01\","
		"\"ltfs_volume_label\":\"TEST VOLUME\","
		"\"ltfs_volume_uuid\":\"11111111-2222-3333-4444-555555555555\","
		"\"index_generation\":42}\n");
	fclose(json);
	return 0;
}

static int test_exit_classes(void)
{
	static const struct {
		int backend_result;
		int expected;
	} cases[] = {
		{LTFS_INFO_BACKEND_NO_MEDIA, LTFS_INFO_NO_MEDIA},
		{LTFS_INFO_BACKEND_UNSUPPORTED, LTFS_INFO_UNSUPPORTED_MEDIA},
		{LTFS_INFO_BACKEND_BUSY, LTFS_INFO_DEVICE_BUSY},
		{LTFS_INFO_BACKEND_READ_ERROR, LTFS_INFO_READ_FAILURE},
	};
	size_t index;
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
		struct fake_backend fake = {
			.result = cases[index].backend_result,
			.fail_call = 1,
		};
		struct ltfs_info_backend backend = backend_for(&fake);
		struct ltfs_info_identity resolved = identity();
		struct ltfs_info_record record;
		CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
			&backend, &record), cases[index].expected);
		CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
		if (fake.opened)
			CHECK_TRUE(fake.closed);
	}
	return 0;
}

static int test_read_failure_at_every_boundary(void)
{
	int fail_call;
	for (fail_call = 1; fail_call <= 10; ++fail_call) {
		struct fake_backend fake = {
			.result = LTFS_INFO_BACKEND_READ_ERROR,
			.fail_call = fail_call,
		};
		struct ltfs_info_backend backend = backend_for(&fake);
		struct ltfs_info_identity resolved = identity();
		struct ltfs_info_record record;
		CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
			&backend, &record), LTFS_INFO_READ_FAILURE);
		CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
		if (fail_call > 1)
			CHECK_TRUE(fake.opened && fake.closed);
		else
			CHECK_TRUE(!fake.opened && !fake.closed);
	}
	return 0;
}

static int test_backend_must_guarantee_read_only(void)
{
	struct fake_backend fake = {0};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;
	backend.read_only_guaranteed = false;
	CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
		&backend, &record), LTFS_INFO_READ_FAILURE);
	CHECK_TRUE(!fake.opened);
	return 0;
}

static void put16(unsigned char *value, uint16_t number)
{
	value[0] = (unsigned char)(number >> 8);
	value[1] = (unsigned char)number;
}

static void put32(unsigned char *value, uint32_t number)
{
	value[0] = (unsigned char)(number >> 24);
	value[1] = (unsigned char)(number >> 16);
	value[2] = (unsigned char)(number >> 8);
	value[3] = (unsigned char)number;
}

static void put64(unsigned char *value, uint64_t number)
{
	put32(value, (uint32_t)(number >> 32));
	put32(value + 4, (uint32_t)number);
}

static int test_mam_and_capacity_parsers(void)
{
	unsigned char coherency[75] = {0};
	unsigned char page[44] = {0};
	unsigned char response[12] = {0};
	unsigned char attribute[8] = {0};
	unsigned char cdb[10] = {0};
	char uuid[LTFS_INFO_UUID_SIZE];
	uint64_t generation = 0;
	struct ltfs_info_capacity capacity;

	put16(coherency, 0x080c);
	put16(coherency + 3, 0x46);
	coherency[5] = 8;
	put64(coherency + 14, 42);
	put16(coherency + 30, 43);
	memcpy(coherency + 32, "LTFS", 4);
	memcpy(coherency + 37,
		"11111111-2222-3333-4444-555555555555", 36);
	coherency[74] = 1;
	CHECK_INT_EQ(ltfs_info_parse_coherency(coherency, sizeof(coherency),
		uuid, &generation), 0);
	CHECK_STR_EQ(uuid, "11111111-2222-3333-4444-555555555555");
	CHECK_INT_EQ(generation, 42);
	CHECK_TRUE(ltfs_info_parse_coherency(coherency,
		sizeof(coherency) - 1, uuid, &generation) < 0);
	coherency[37] = 'z';
	CHECK_TRUE(ltfs_info_parse_coherency(coherency, sizeof(coherency),
		uuid, &generation) < 0);

	page[0] = 0x17;
	put16(page + 2, 40);
	put16(page + 4, 0x0202);
	page[7] = 16;
	page[8] = 7;
	put32(page + 12, 3000);
	page[16] = 7;
	put32(page + 20, 4000);
	put16(page + 24, 0x0204);
	page[27] = 16;
	page[28] = 7;
	put32(page + 32, 1000);
	page[36] = 7;
	put32(page + 40, 2000);
	CHECK_INT_EQ(ltfs_info_parse_capacity_page17(page, sizeof(page),
		&capacity), 0);
	CHECK_INT_EQ(capacity.remaining_partition0_mib, 953);
	CHECK_INT_EQ(capacity.remaining_partition1_mib, 1907);
	CHECK_INT_EQ(capacity.maximum_partition0_mib, 2861);
	CHECK_INT_EQ(capacity.maximum_partition1_mib, 3814);
	put16(page + 24, 0x0203);
	put32(page + 32, 500);
	put32(page + 40, 1000);
	CHECK_INT_EQ(ltfs_info_parse_capacity_page17(page, sizeof(page),
		&capacity), 0);
	CHECK_INT_EQ(capacity.remaining_partition0_mib, 2384);
	CHECK_INT_EQ(capacity.remaining_partition1_mib, 2861);
	put32(page + 32, UINT32_MAX);
	CHECK_INT_EQ(ltfs_info_parse_capacity_page17(page, sizeof(page),
		&capacity), LTFS_INFO_BACKEND_UNSUPPORTED);
	put32(page + 32, 500);
	put32(page + 40, UINT32_MAX);
	CHECK_INT_EQ(ltfs_info_parse_capacity_page17(page, sizeof(page),
		&capacity), LTFS_INFO_BACKEND_UNSUPPORTED);
	put16(page + 24, 0x0204);
	put32(page + 32, 1000);
	put32(page + 40, 2000);
	CHECK_TRUE(ltfs_info_parse_capacity_page17(page,
		sizeof(page) - 1, &capacity) < 0);
	page[27] = 15;
	CHECK_TRUE(ltfs_info_parse_capacity_page17(page, sizeof(page),
		&capacity) < 0);

	/* READ ATTRIBUTE has a four-byte list header, then a descriptor with
	 * a two-byte value length at descriptor bytes 3-4. */
	put32(response, 8);
	put16(response + 4, ATTR_APPLICATION_NAME);
	response[6] = 1;
	put16(response + 7, 3);
	memcpy(response + 9, "LTO", 3);
	CHECK_INT_EQ(ltfs_info_extract_attribute_response(response,
		sizeof(response), ATTR_APPLICATION_NAME, attribute,
		sizeof(attribute)), 0);
	CHECK_INT_EQ(attribute[0], 0x08);
	CHECK_INT_EQ(attribute[1], 0x01);
	CHECK_INT_EQ(attribute[3], 0);
	CHECK_INT_EQ(attribute[4], 3);
	CHECK_TRUE(!memcmp(attribute + 5, "LTO", 3));
	response[7] = 1;
	CHECK_TRUE(ltfs_info_extract_attribute_response(response,
		sizeof(response), ATTR_APPLICATION_NAME, attribute,
		sizeof(attribute)) < 0);
	response[7] = 0;
	put32(response, 9);
	CHECK_INT_EQ(ltfs_info_extract_attribute_response(response,
		sizeof(response), ATTR_APPLICATION_NAME, attribute,
		sizeof(attribute)), 0);
	put32(response, 7);
	CHECK_TRUE(ltfs_info_extract_attribute_response(response,
		sizeof(response), ATTR_APPLICATION_NAME, attribute,
		sizeof(attribute)) < 0);

	CHECK_INT_EQ(ltfs_info_build_capacity_log_sense_cdb(cdb, 1024), 0);
	CHECK_INT_EQ(cdb[0], OP_LOG_SENSE);
	CHECK_INT_EQ(cdb[2], 0x57);
	CHECK_INT_EQ(cdb[7], 4);
	CHECK_INT_EQ(cdb[8], 0);
	CHECK_TRUE(ltfs_info_build_capacity_log_sense_cdb(cdb, 65536) < 0);
	return 0;
}

static int test_medium_serial_descriptor_is_normalized_and_nonempty(void)
{
	unsigned char descriptor[5 + 32] = {0};
	char serial[LTFS_INFO_TEXT_MAX] = {0};

	put16(descriptor, ATTR_MEDIUM_SERIAL_NUMBER);
	descriptor[2] = 1;
	put16(descriptor + 3, 32);
	memset(descriptor + 5, ' ', 32);
	memcpy(descriptor + 7, "SERIAL-0401", 11);
	CHECK_INT_EQ(ltfs_info_parse_text_attribute_descriptor(descriptor,
		sizeof(descriptor), ATTR_MEDIUM_SERIAL_NUMBER, serial,
		sizeof(serial)), 0);
	CHECK_STR_EQ(serial, "SERIAL-0401");

	put16(descriptor, ATTR_VOLUME_IDENTIFIER);
	CHECK_INT_EQ(ltfs_info_parse_text_attribute_descriptor(descriptor,
		sizeof(descriptor), ATTR_MEDIUM_SERIAL_NUMBER, serial,
		sizeof(serial)), LTFS_INFO_BACKEND_READ_ERROR);
	put16(descriptor, ATTR_MEDIUM_SERIAL_NUMBER);
	memset(descriptor + 5, ' ', 32);
	CHECK_INT_EQ(ltfs_info_parse_text_attribute_descriptor(descriptor,
		sizeof(descriptor), ATTR_MEDIUM_SERIAL_NUMBER, serial,
		sizeof(serial)), LTFS_INFO_BACKEND_UNSUPPORTED);
	descriptor[5] = 0x1f;
	CHECK_INT_EQ(ltfs_info_parse_text_attribute_descriptor(descriptor,
		sizeof(descriptor), ATTR_MEDIUM_SERIAL_NUMBER, serial,
		sizeof(serial)), LTFS_INFO_BACKEND_UNSUPPORTED);
	return 0;
}

static int test_optional_capacity_is_null(void)
{
	struct fake_backend fake = {.capacity_unavailable = true};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;
	FILE *json;
	char output[4096] = {0};
	CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
		&backend, &record), LTFS_INFO_READY);
	CHECK_TRUE(!record.capacity_valid);
	CHECK_INT_EQ(ltfs_info_set_config_paths(&record,
		"/dev/tape/by-id/test-drive-nst",
		"/dev/lto-archiver-scsi-test-drive-sg"), 0);
	json = tmpfile();
	CHECK_TRUE(json != NULL);
	CHECK_INT_EQ(ltfs_info_write_json(json, &record), 0);
	rewind(json);
	CHECK_TRUE(fread(output, 1, sizeof(output) - 1, json) > 0);
	CHECK_TRUE(strstr(output, "\"mam_barcode\":\"TEST01\"") != NULL);
	CHECK_TRUE(strstr(output,
		"\"mam_volume_serial\":\"SERIAL-TEST-01\"") != NULL);
	CHECK_TRUE(strstr(output,
		"\"ltfs_volume_label\":\"TEST VOLUME\"") != NULL);
	fclose(json);
	return 0;
}

static int test_pre_format_non_ltfs_returns_required_identity_only(void)
{
	struct fake_backend fake = {.non_ltfs = true};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;

	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record), LTFS_INFO_READY);
	CHECK_TRUE(fake.opened && fake.closed && record.ready);
	CHECK_TRUE(record.mam_barcode_valid);
	CHECK_STR_EQ(record.mam_barcode, "TEST01");
	CHECK_TRUE(record.mam_volume_serial_valid);
	CHECK_STR_EQ(record.mam_volume_serial, "SERIAL-TEST-01");
	CHECK_TRUE(!record.mam_volume_label_valid);
	CHECK_TRUE(!record.volume_uuid_valid);
	CHECK_TRUE(!record.index_generation_valid);
	CHECK_INT_EQ(record.media_state, LTFS_INFO_MEDIA_UNIDENTIFIED);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.app_name_unavailable = true,
		.barcode_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record), LTFS_INFO_READY);
	CHECK_TRUE(!record.mam_barcode_valid);
	CHECK_TRUE(record.mam_volume_serial_valid);
	CHECK_INT_EQ(record.media_state, LTFS_INFO_MEDIA_UNIDENTIFIED);
	CHECK_TRUE(!record.mam_volume_label_valid && !record.volume_uuid_valid &&
		!record.index_generation_valid);
	{
		FILE *json = tmpfile();
		char output[4096] = {0};
		CHECK_TRUE(json != NULL);
		CHECK_INT_EQ(ltfs_info_set_config_paths(&record,
			"/dev/tape/by-id/test-drive-nst",
			"/dev/lto-archiver-scsi-test-drive-sg"), 0);
		CHECK_INT_EQ(ltfs_info_write_json(json, &record), 0);
		rewind(json);
		CHECK_TRUE(fread(output, 1, sizeof(output) - 1, json) > 0);
		CHECK_TRUE(!strncmp(output,
			"{\"schema\":2,\"media_state\":\"unidentified\"", 39));
		fclose(json);
	}
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.non_ltfs = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
		&backend, &record), LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.app_name_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
		&backend, &record), LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
	return 0;
}

static int test_pre_format_ltfs_missing_barcode_is_unidentified(void)
{
	struct fake_backend fake = {0};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;

	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record), LTFS_INFO_READY);
	CHECK_TRUE(record.mam_barcode_valid && record.mam_volume_serial_valid);
	CHECK_TRUE(record.mam_volume_label_valid && record.volume_uuid_valid &&
		record.index_generation_valid);
	CHECK_STR_EQ(record.mam_volume_label, "TEST VOLUME");
	CHECK_STR_EQ(record.volume_uuid,
		"11111111-2222-3333-4444-555555555555");
	CHECK_TRUE(record.index_generation == 42);
	CHECK_INT_EQ(record.media_state, LTFS_INFO_MEDIA_LTFS);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.barcode_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record), LTFS_INFO_READY);
	CHECK_INT_EQ(record.media_state, LTFS_INFO_MEDIA_UNIDENTIFIED);
	CHECK_TRUE(!record.mam_barcode_valid);
	CHECK_TRUE(record.mam_volume_serial_valid);
	CHECK_STR_EQ(record.mam_volume_serial, "SERIAL-TEST-01");
	CHECK_TRUE(!record.mam_volume_label_valid);
	CHECK_TRUE(!record.volume_uuid_valid);
	CHECK_TRUE(!record.index_generation_valid);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
	CHECK_INT_EQ(assert_medium_serial_was_read(&fake), 0);
	{
		FILE *json = tmpfile();
		char output[4096] = {0};
		CHECK_TRUE(json != NULL);
		CHECK_INT_EQ(ltfs_info_set_config_paths(&record,
			"/dev/tape/by-id/test-drive-nst",
			"/dev/lto-archiver-scsi-test-drive-sg"), 0);
		CHECK_INT_EQ(ltfs_info_write_json(json, &record), 0);
		rewind(json);
		CHECK_TRUE(fread(output, 1, sizeof(output) - 1, json) > 0);
		CHECK_STR_EQ(output,
			"{\"schema\":2,\"media_state\":\"unidentified\","
			"\"tape_by_id\":\"/dev/tape/by-id/test-drive-nst\","
			"\"scsi_by_id\":\"/dev/lto-archiver-scsi-test-drive-sg\","
			"\"drive_serial\":\"DRIVE-TEST-01\","
			"\"mam_barcode\":null,"
			"\"mam_volume_serial\":\"SERIAL-TEST-01\","
			"\"ltfs_volume_label\":null,"
			"\"ltfs_volume_uuid\":null,"
			"\"index_generation\":null}\n");
		fclose(json);
	}

	fake = (struct fake_backend){.barcode_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect("/dev/sg-test", &resolved,
		&backend, &record), LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.coherency_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record),
		LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.volume_label_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record),
		LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.app_name_unavailable = true,
		.volume_serial_unavailable = true};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record),
		LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.non_ltfs = true};
	strcpy(fake.volume_serial, "   ");
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record),
		LTFS_INFO_UNSUPPORTED_MEDIA);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);

	fake = (struct fake_backend){.result = LTFS_INFO_BACKEND_READ_ERROR,
		.fail_call = 4};
	backend = backend_for(&fake);
	CHECK_INT_EQ(ltfs_info_collect_mode("/dev/sg-test", &resolved,
		&backend, LTFS_INFO_MODE_PRE_FORMAT, &record),
		LTFS_INFO_READ_FAILURE);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
	return 0;
}

static int test_installed_self_test_fixture(void)
{
	FILE *json = tmpfile();
	char output[4096] = {0};
	CHECK_TRUE(json != NULL);
	CHECK_INT_EQ(ltfs_info_run_self_test_fixture(json, "ready"),
		LTFS_INFO_READY);
	rewind(json);
	CHECK_TRUE(fread(output, 1, sizeof(output) - 1, json) > 0);
	CHECK_TRUE(strstr(output,
		"\"tape_by_id\":\"/dev/tape/by-id/self-test-nst\"") != NULL);
	CHECK_TRUE(strstr(output,
		"\"scsi_by_id\":\"/dev/lto-archiver-scsi-self-test-sg\"") != NULL);
	fclose(json);
	json = tmpfile();
	CHECK_TRUE(json != NULL);
	CHECK_INT_EQ(ltfs_info_run_self_test_fixture(json, "pre-format"),
		LTFS_INFO_READY);
	rewind(json);
	memset(output, 0, sizeof(output));
	CHECK_TRUE(fread(output, 1, sizeof(output) - 1, json) > 0);
	CHECK_TRUE(strstr(output, "\"mam_barcode\":\"SELFTEST01\"") != NULL);
	CHECK_TRUE(strstr(output,
		"\"mam_volume_serial\":\"SELFTEST-SERIAL-01\"") != NULL);
	CHECK_TRUE(strstr(output, "\"ltfs_volume_label\":null") != NULL);
	CHECK_TRUE(strstr(output, "\"ltfs_volume_uuid\":null") != NULL);
	CHECK_TRUE(strstr(output, "\"index_generation\":null") != NULL);
	fclose(json);
	CHECK_INT_EQ(ltfs_info_run_self_test_fixture(stdout, "unknown"), 2);
	return 0;
}

static int test_discovery_emits_exact_provisionable_config(void)
{
	struct ltfs_info_identity resolved = identity();
	FILE *json = tmpfile();
	char output[2048] = {0};
	CHECK_TRUE(json != NULL);
	CHECK_INT_EQ(ltfs_info_write_device_config_json(json,
		"/dev/tape/by-id/test-drive-nst",
		"/dev/lto-archiver-scsi-test-drive-sg", &resolved), 0);
	rewind(json);
	CHECK_TRUE(fread(output, 1, sizeof(output) - 1, json) > 0);
	CHECK_STR_EQ(output,
		"{\"nst_path\":\"/dev/tape/by-id/test-drive-nst\","
		"\"sg_path\":\"/dev/lto-archiver-scsi-test-drive-sg\","
		"\"serial\":\"DRIVE-TEST-01\","
		"\"wwid\":\"naa.5000test\"}\n");
	fclose(json);
	json = tmpfile();
	CHECK_TRUE(json != NULL);
	CHECK_TRUE(ltfs_info_write_device_config_json(json, "/dev/nst0",
		"/dev/sg0", &resolved) < 0);
	fclose(json);
	json = tmpfile();
	CHECK_TRUE(json != NULL);
	CHECK_TRUE(ltfs_info_write_device_config_json(json,
		"/dev/tape/by-id/test-drive-nst",
		"/dev/lto-archiver/by-id/legacy-sg", &resolved) < 0);
	fclose(json);
	return 0;
}

#ifdef LTFS_INFO_TEST_CLI
static int fixture_line(FILE *stream, char *value, size_t size)
{
	size_t length;
	if (!stream || !value || size < 2 || !fgets(value, size, stream))
		return -EINVAL;
	length = strlen(value);
	if (length < 2 || value[length - 1] != '\n')
		return -EINVAL;
	value[--length] = '\0';
	if (value[length - 1] == '\r')
		return -EINVAL;
	return 0;
}

static int emit_file_fixture(const char *path)
{
	struct fake_backend fake = {.non_ltfs = true};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;
	char media_type[LTFS_INFO_TEXT_MAX];
	FILE *stream;
	int result;

	stream = fopen(path, "rb");
	if (!stream)
		return LTFS_INFO_READ_FAILURE;
	if (fixture_line(stream, media_type, sizeof(media_type)) < 0 ||
		strcmp(media_type, "NON-LTFS") ||
		fixture_line(stream, fake.barcode, sizeof(fake.barcode)) < 0 ||
		fixture_line(stream, fake.volume_serial,
			sizeof(fake.volume_serial)) < 0 ||
		fgetc(stream) != EOF || ferror(stream)) {
		fclose(stream);
		return LTFS_INFO_READ_FAILURE;
	}
	if (fclose(stream) == EOF)
		return LTFS_INFO_READ_FAILURE;
	result = ltfs_info_collect_mode("/dev/sg-test", &resolved, &backend,
		LTFS_INFO_MODE_PRE_FORMAT, &record);
	if (result == LTFS_INFO_READY && ltfs_info_set_config_paths(&record,
		"/dev/tape/by-id/test-drive-nst",
		"/dev/lto-archiver-scsi-test-drive-sg") < 0)
		return LTFS_INFO_IDENTITY_MISMATCH;
	if (result == LTFS_INFO_READY && ltfs_info_write_json(stdout, &record) < 0)
		return LTFS_INFO_READ_FAILURE;
	return result;
}

static int emit_fixture(const char *name)
{
	struct fake_backend fake = {0};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_record record;
	int result;
	if (!strcmp(name, "no-media"))
		fake.result = LTFS_INFO_BACKEND_NO_MEDIA;
	else if (!strcmp(name, "unsupported"))
		fake.result = LTFS_INFO_BACKEND_UNSUPPORTED;
	else if (!strcmp(name, "busy"))
		fake.result = LTFS_INFO_BACKEND_BUSY;
	else if (!strcmp(name, "read-failure"))
		fake.result = LTFS_INFO_BACKEND_READ_ERROR;
	else if (!strcmp(name, "partial"))
		fake.optional_unavailable = true;
	else if (!strcmp(name, "pre-format"))
		fake.non_ltfs = true;
	else if (!strcmp(name, "pre-format-missing-volume-serial")) {
		fake.non_ltfs = true;
		fake.volume_serial_unavailable = true;
	}
	else if (!strcmp(name, "pre-format-blank-medium-serial")) {
		fake.non_ltfs = true;
		strcpy(fake.volume_serial, "   ");
	}
	else if (!strcmp(name, "pre-format-invalid-medium-serial")) {
		fake.non_ltfs = true;
		strcpy(fake.volume_serial, "BAD\x1fSERIAL");
	}
	else if (!strcmp(name, "pre-format-ltfs"))
		fake.result = 0;
	else if (!strcmp(name, "pre-format-ltfs-missing-barcode"))
		fake.barcode_unavailable = true;
	else if (!strcmp(name, "normal-ltfs-missing-barcode"))
		fake.barcode_unavailable = true;
	else if (!strcmp(name, "identity-mismatch"))
		return LTFS_INFO_IDENTITY_MISMATCH;
	else if (!strcmp(name, "ready"))
		fake.result = 0;
	else
		return 2;
	if (fake.result)
		fake.fail_call = 1;
	result = (!strcmp(name, "pre-format") ||
		!strcmp(name, "pre-format-missing-volume-serial") ||
		!strcmp(name, "pre-format-blank-medium-serial") ||
		!strcmp(name, "pre-format-invalid-medium-serial") ||
		!strcmp(name, "pre-format-ltfs") ||
		!strcmp(name, "pre-format-ltfs-missing-barcode")) ?
		ltfs_info_collect_mode("/dev/sg-test", &resolved, &backend,
			LTFS_INFO_MODE_PRE_FORMAT, &record) :
		ltfs_info_collect("/dev/sg-test", &resolved, &backend, &record);
	if (result == LTFS_INFO_READY && ltfs_info_set_config_paths(&record,
		"/dev/tape/by-id/test-drive-nst",
		"/dev/lto-archiver-scsi-test-drive-sg") < 0)
		return LTFS_INFO_IDENTITY_MISMATCH;
	if (result == LTFS_INFO_READY)
		CHECK_INT_EQ(ltfs_info_write_json(stdout, &record), 0);
	return result;
}
#endif

static int test_capacity_inquiry_selects_vendor_specific_page(void)
{
	const struct { const char *vendor, *product; int expected; } cases[] = {
		{"HP", "Ultrium 5-SCSI", 0x31}, {"HP", "Ultrium 6-SCSI", 0x31},
		{"HP", "Ultrium 7-SCSI", 0x17}, {"HPE", "Ultrium 8-SCSI", 0x17},
		{"IBM", "ULT3580-HH5", 0x31}, {"IBM", "ULT3580-TD6", 0x17},
		{"IBM", "ULTRIUM-HHA", 0x17}, {"IBM", "HH LTO Gen 9", 0x17},
		{"QUANTUM", "ULTRIUM-HH5", 0x31}, {"QUANTUM", "ULTRIUM 6", 0x17},
		{"HP", "Ultrium 4-SCSI", -1}, {"OTHER", "Ultrium 6-SCSI", -1},
		{"HP", "Ultrium 6-SCSIX", -1}, {"IBM", "BAD", -1},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
		unsigned char inquiry[96] = {1, 0, 0, 0, 91};
		unsigned char page = 0;
		memset(inquiry + 8, ' ', 24);
		memcpy(inquiry + 8, cases[i].vendor, strlen(cases[i].vendor));
		memcpy(inquiry + 16, cases[i].product, strlen(cases[i].product));
		if (cases[i].expected < 0)
			CHECK_INT_EQ(ltfs_info_select_capacity_page(inquiry, sizeof(inquiry),
				&page), LTFS_INFO_BACKEND_UNSUPPORTED);
		else {
			CHECK_INT_EQ(ltfs_info_select_capacity_page(inquiry, sizeof(inquiry),
				&page), 0);
			CHECK_INT_EQ(page, cases[i].expected);
		}
		CHECK_INT_EQ(ltfs_info_select_capacity_page(inquiry, 35, &page),
			LTFS_INFO_BACKEND_READ_ERROR);
		inquiry[0] = 0; /* A disk is never a supported tape. */
		CHECK_INT_EQ(ltfs_info_select_capacity_page(inquiry, sizeof(inquiry),
			&page), LTFS_INFO_BACKEND_UNSUPPORTED);
	}
	return 0;
}

static int test_capacity_requires_sealed_identity_and_stable_session(void)
{
	struct ltfs_info_identity resolved = identity();
	struct ltfs_info_expected_medium expected = {
		.drive_serial = "DRIVE-TEST-01", .medium_serial = "SERIAL-TEST-01",
		.volume_label = "TEST VOLUME",
		.volume_uuid = "11111111-2222-3333-4444-555555555555",
		.index_generation = 42,
	};
	struct fake_backend fake = {0};
	struct ltfs_info_backend backend = backend_for(&fake);
	struct ltfs_info_capacity_report report;
	int total_calls;
	CHECK_INT_EQ(ltfs_info_collect_capacity("/dev/sg-test", &resolved,
		&backend, &expected, &report), 0);
	CHECK_TRUE(report.identity_verified_before_after);
	CHECK_INT_EQ(report.record.capacity.remaining_partition1_mib, 20);
	CHECK_INT_EQ(fake.open_count, 1);
	CHECK_INT_EQ(fake.close_count, 1);
	CHECK_INT_EQ(fake.capacity_count, 1);
	CHECK_INT_EQ(assert_no_mutating_opcode(&fake), 0);
	total_calls = fake.call_count;
	for (int stage = 1; stage <= total_calls; ++stage) {
		memset(&fake, 0, sizeof(fake));
		fake.fail_call = stage;
		fake.result = LTFS_INFO_BACKEND_READ_ERROR;
		CHECK_INT_EQ(ltfs_info_collect_capacity("/dev/sg-test", &resolved,
			&backend, &expected, &report), LTFS_INFO_READ_FAILURE);
		CHECK_TRUE(!report.identity_verified_before_after);
		CHECK_TRUE(!report.record.capacity_valid);
		CHECK_INT_EQ(fake.close_count, stage == 1 ? 0 : 1);
	}
	for (int change = 0; change < 5; ++change) {
		memset(&fake, 0, sizeof(fake));
		fake.changed_attribute = change == 0 ? ATTR_MEDIUM_SERIAL_NUMBER :
			change == 1 ? ATTR_VOLUME_LABEL : change == 2 ? ATTR_BARCODE : 0;
		fake.changed_uuid = change == 3;
		fake.changed_generation = change == 4;
		CHECK_INT_EQ(ltfs_info_collect_capacity("/dev/sg-test", &resolved,
			&backend, &expected, &report), LTFS_INFO_IDENTITY_MISMATCH);
		CHECK_TRUE(!report.identity_verified_before_after);
		CHECK_INT_EQ(fake.close_count, 1);
	}
	memset(&fake, 0, sizeof(fake));
	fake.close_result = LTFS_INFO_BACKEND_READ_ERROR;
	CHECK_INT_EQ(ltfs_info_collect_capacity("/dev/sg-test", &resolved,
		&backend, &expected, &report), LTFS_INFO_READ_FAILURE);
	CHECK_TRUE(!report.record.capacity_valid);
	memset(&fake, 0, sizeof(fake));
	expected.medium_serial = "WRONG";
	CHECK_INT_EQ(ltfs_info_collect_capacity("/dev/sg-test", &resolved,
		&backend, &expected, &report), LTFS_INFO_IDENTITY_MISMATCH);
	CHECK_INT_EQ(fake.capacity_count, 0);
	memset(&fake, 0, sizeof(fake));
	expected.medium_serial = "SERIAL-TEST-01";
	expected.volume_uuid = "invalid";
	CHECK_INT_EQ(ltfs_info_collect_capacity("/dev/sg-test", &resolved,
		&backend, &expected, &report), LTFS_INFO_READ_FAILURE);
	CHECK_INT_EQ(fake.open_count, 0);
	return 0;
}

int main(int argc, char **argv)
{
#ifdef LTFS_INFO_TEST_CLI
	if (argc == 3 && !strcmp(argv[1], "--fixture"))
		return emit_fixture(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "--file-fixture"))
		return emit_file_fixture(argv[2]);
#else
	(void)argc;
	(void)argv;
#endif
	CHECK_INT_EQ(test_success(), 0);
	CHECK_INT_EQ(test_capacity_inquiry_selects_vendor_specific_page(), 0);
	CHECK_INT_EQ(test_capacity_requires_sealed_identity_and_stable_session(), 0);
	{
		FILE *diagnostic = tmpfile();
		char output[4096] = {0};
		CHECK_TRUE(diagnostic != NULL);
		CHECK_INT_EQ(ltfs_info_run_self_test_fixture(diagnostic, "capacity"), 0);
		rewind(diagnostic);
		CHECK_TRUE(fread(output, 1, sizeof(output) - 1, diagnostic) > 0);
		CHECK_TRUE(strstr(output, "\"kind\":\"ltfs-capacity\"") != NULL);
		CHECK_TRUE(strstr(output, "\"unit\":\"MiB\"") != NULL);
		CHECK_TRUE(strstr(output, "\"capacity_offset\":0") != NULL);
		CHECK_TRUE(strstr(output, "\"remaining_partition1_mib\":2") != NULL);
		CHECK_TRUE(strstr(output, "\"identity_verified_before_after\":true") != NULL);
		CHECK_INT_EQ(fclose(diagnostic), 0);
	}
	CHECK_INT_EQ(test_exit_classes(), 0);
	CHECK_INT_EQ(test_read_failure_at_every_boundary(), 0);
	CHECK_INT_EQ(test_backend_must_guarantee_read_only(), 0);
	CHECK_INT_EQ(test_mam_and_capacity_parsers(), 0);
	CHECK_INT_EQ(test_medium_serial_descriptor_is_normalized_and_nonempty(), 0);
	CHECK_INT_EQ(test_optional_capacity_is_null(), 0);
	CHECK_INT_EQ(test_pre_format_non_ltfs_returns_required_identity_only(), 0);
	CHECK_INT_EQ(test_pre_format_ltfs_missing_barcode_is_unidentified(), 0);
	CHECK_INT_EQ(test_installed_self_test_fixture(), 0);
	CHECK_INT_EQ(test_discovery_emits_exact_provisionable_config(), 0);
	return 0;
}
