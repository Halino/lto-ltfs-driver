/* SPDX-License-Identifier: LGPL-2.1-only */
/* Downstream modifications: 2026-08 through 2026-09; see LGPL-NOTICE. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "ltfs_info.h"
#include "device_identity.h"
#include "tape_ops.h"
#include "tape_drivers/tape_drivers.h"
#include "tape_drivers/linux/sg/sg_capacity.h"

#ifndef LTFS_INFO_NO_MAIN
#include "device_guard.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <scsi/sg.h>
#include <sys/ioctl.h>
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION "unknown"
#endif

#define LTFS_INFO_APP_NAME 0x0801
#define LTFS_INFO_APP_VERSION 0x0802
#define LTFS_INFO_VOLUME_LABEL 0x0803
#define LTFS_INFO_BARCODE 0x0806
#define LTFS_INFO_COHERENCY 0x080c
#define LTFS_INFO_COHERENCY_SIZE 75
#define LTFS_INFO_TIMEOUT_MS 30000U

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_INQUIRY 0x12
#define SCSI_LOG_SENSE 0x4d
#define SCSI_READ_ATTRIBUTE 0x8c

struct linux_sg_backend {
	int fd;
	dev_t expected_rdev;
	bool capacity_diagnostic;
	const char *expected_serial;
	unsigned char capacity_page;
	size_t transferred;
};

int ltfs_info_select_capacity_page(const unsigned char *inquiry, size_t size,
	unsigned char *page)
{
	char vendor[9] = {0}, product[17] = {0}, candidate[32];
	int vendor_id = VENDOR_UNKNOWN, generation = 0;
	if (!page)
		return LTFS_INFO_BACKEND_READ_ERROR;
	*page = 0;
	if (!inquiry || size < 36 || inquiry[4] < 31)
		return LTFS_INFO_BACKEND_READ_ERROR;
	if (inquiry[0] != 1)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	if (ltfs_device_identity_normalize_text(vendor, sizeof(vendor), inquiry + 8, 8) ||
		ltfs_device_identity_normalize_text(product, sizeof(product), inquiry + 16, 16))
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	if (!strcmp(vendor, "HP") || !strcmp(vendor, "HPE"))
		vendor_id = VENDOR_HP;
	else if (!strcmp(vendor, "IBM"))
		vendor_id = VENDOR_IBM;
	else if (!strcmp(vendor, "QUANTUM"))
		vendor_id = VENDOR_QUANTUM;
	/* Recognize only explicit LTO inquiry product families; never guess for an
	 * unknown device. Page selection itself is shared with sg_remaining_capacity. */
	for (int gen = 5; gen <= 10 && !generation; ++gen) {
		if (vendor_id == VENDOR_HP && gen <= 9) {
			snprintf(candidate, sizeof(candidate), "Ultrium %d-SCSI", gen);
			if (!strcmp(product, candidate)) generation = gen;
		} else if (vendor_id == VENDOR_IBM) {
			const char *families[] = {"ULT3580-TD", "ULT3580-HH", "ULTRIUM-TD", "ULTRIUM-HH"};
			for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
				snprintf(candidate, sizeof(candidate), "%s%c", families[i], gen == 10 ? 'A' : '0' + gen);
				if (!strcmp(product, candidate)) generation = gen;
			}
			if (gen <= 9) {
				snprintf(candidate, sizeof(candidate), "HH LTO Gen %d", gen);
				if (!strcmp(product, candidate)) generation = gen;
			}
		} else if (vendor_id == VENDOR_QUANTUM && gen <= 9) {
			snprintf(candidate, sizeof(candidate), "ULTRIUM-HH%d", gen);
			if (!strcmp(product, candidate)) generation = gen;
			if (gen <= 6) {
				snprintf(candidate, sizeof(candidate), "ULTRIUM %d", gen);
				if (!strcmp(product, candidate)) generation = gen;
			}
		}
	}
	if (!generation)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	*page = sg_capacity_uses_page31(vendor_id,
		(DRIVE_LTO5 & ~0xff) | generation) ? 0x31 : 0x17;
	return 0;
}

static int copy_text(char *destination, size_t size, const char *source)
{
	size_t length;
	if (!destination || !source || size == 0)
		return -EINVAL;
	length = strlen(source);
	if (length >= size)
		return -ENAMETOOLONG;
	memcpy(destination, source, length + 1);
	return 0;
}

static int map_backend_result(int result)
{
	switch (result) {
	case LTFS_INFO_BACKEND_NO_MEDIA:
		return LTFS_INFO_NO_MEDIA;
	case LTFS_INFO_BACKEND_UNSUPPORTED:
		return LTFS_INFO_UNSUPPORTED_MEDIA;
	case LTFS_INFO_BACKEND_BUSY:
		return LTFS_INFO_DEVICE_BUSY;
	default:
		return LTFS_INFO_READ_FAILURE;
	}
}

static bool normalized_identity_text(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	size_t length;
	if (!value || !*value)
		return false;
	length = strlen(value);
	if (value[0] == ' ' || value[length - 1] == ' ')
		return false;
	for (; *cursor; ++cursor)
		if (*cursor < 0x20 || *cursor > 0x7e)
			return false;
	return true;
}

static bool backend_complete(const struct ltfs_info_backend *backend)
{
	return backend && backend->read_only_guaranteed && backend->open &&
		backend->close && backend->inquiry && backend->test_unit_ready &&
		backend->read_text_attribute && backend->read_coherency &&
		backend->remaining_capacity;
}

static int collect_opened(
	const struct ltfs_info_identity *identity,
	struct ltfs_info_backend *backend, enum ltfs_info_mode mode,
	struct ltfs_info_record *record, bool read_capacity)
{
	int result;
	bool is_ltfs;
	memset(record, 0, sizeof(*record));
	if (copy_text(record->build_name, sizeof(record->build_name),
		"lto-ltfs") < 0 ||
		copy_text(record->build_version, sizeof(record->build_version),
			PACKAGE_VERSION) < 0)
		return LTFS_INFO_BACKEND_READ_ERROR;
	record->identity = *identity;
	result = backend->inquiry(backend->context);
	if (result)
		goto out;
	result = backend->test_unit_ready(backend->context);
	if (result)
		goto out;
	record->ready = true;
	result = backend->read_text_attribute(backend->context,
		LTFS_INFO_APP_NAME, record->mam_application_name,
		sizeof(record->mam_application_name));
	if (result && (mode != LTFS_INFO_MODE_PRE_FORMAT ||
		result != LTFS_INFO_BACKEND_UNSUPPORTED))
		goto out;
	record->mam_application_name_valid = result == 0;
	is_ltfs = result == 0 && !strcmp(record->mam_application_name, "LTFS");
	if (!is_ltfs && mode != LTFS_INFO_MODE_PRE_FORMAT) {
		result = LTFS_INFO_BACKEND_UNSUPPORTED;
		goto out;
	}
	record->media_state = is_ltfs ? LTFS_INFO_MEDIA_LTFS :
		LTFS_INFO_MEDIA_UNIDENTIFIED;
	if (is_ltfs) {
		result = backend->read_text_attribute(backend->context,
			LTFS_INFO_APP_VERSION, record->mam_application_version,
			sizeof(record->mam_application_version));
		if (result && result != LTFS_INFO_BACKEND_UNSUPPORTED)
			goto out;
		record->mam_application_version_valid = result == 0;
		result = backend->read_text_attribute(backend->context,
			LTFS_INFO_VOLUME_LABEL, record->mam_volume_label,
			sizeof(record->mam_volume_label));
		if (result && result != LTFS_INFO_BACKEND_UNSUPPORTED)
			goto out;
		record->mam_volume_label_valid = result == 0;
		if (!record->mam_volume_label_valid) {
			result = LTFS_INFO_BACKEND_UNSUPPORTED;
			goto out;
		}
	}
	result = backend->read_text_attribute(backend->context,
		LTFS_INFO_BARCODE, record->mam_barcode,
		sizeof(record->mam_barcode));
	if (result && result != LTFS_INFO_BACKEND_UNSUPPORTED)
		goto out;
	record->mam_barcode_valid = result == 0 &&
		normalized_identity_text(record->mam_barcode);
	result = backend->read_text_attribute(backend->context,
		TC_MAM_MEDIUM_SERIAL_NUMBER, record->mam_volume_serial,
		sizeof(record->mam_volume_serial));
	if (result && result != LTFS_INFO_BACKEND_UNSUPPORTED)
		goto out;
	record->mam_volume_serial_valid = result == 0 &&
		normalized_identity_text(record->mam_volume_serial);
	if (!record->mam_volume_serial_valid) {
		result = LTFS_INFO_BACKEND_UNSUPPORTED;
		goto out;
	}
	if (!is_ltfs) {
		result = 0;
		goto out;
	}
	if (mode != LTFS_INFO_MODE_PRE_FORMAT && !record->mam_barcode_valid) {
		result = LTFS_INFO_BACKEND_UNSUPPORTED;
		goto out;
	}
	result = backend->read_coherency(backend->context,
		record->volume_uuid, &record->index_generation);
	if (result || !record->index_generation) {
		if (!result)
			result = LTFS_INFO_BACKEND_UNSUPPORTED;
		goto out;
	}
	record->volume_uuid_valid = true;
	record->index_generation_valid = true;
	if (!record->mam_barcode_valid) {
		record->media_state = LTFS_INFO_MEDIA_UNIDENTIFIED;
		record->mam_volume_label_valid = false;
		record->volume_uuid_valid = false;
		record->index_generation_valid = false;
		result = 0;
		goto out;
	}
	if (!read_capacity)
		return 0;
	result = backend->remaining_capacity(backend->context, &record->capacity);
	if (result == LTFS_INFO_BACKEND_UNSUPPORTED)
		result = 0;
	else if (!result)
		record->capacity_valid = true;
out:
	return result;
}

int ltfs_info_collect_mode(const char *sg_path,
	const struct ltfs_info_identity *identity,
	struct ltfs_info_backend *backend, enum ltfs_info_mode mode,
	struct ltfs_info_record *record)
{
	int result, close_result;
	if (!sg_path || !identity || !record || !backend_complete(backend) ||
		(mode != LTFS_INFO_MODE_UNMOUNTED && mode != LTFS_INFO_MODE_PRE_FORMAT))
		return LTFS_INFO_READ_FAILURE;
	memset(record, 0, sizeof(*record));
	result = backend->open(backend->context, sg_path);
	if (result)
		return map_backend_result(result);
	result = collect_opened(identity, backend, mode, record, true);
	close_result = backend->close(backend->context);
	if (!result && close_result)
		result = close_result;
	return result ? map_backend_result(result) : LTFS_INFO_READY;
}

static bool info_uuid_valid(const char *uuid);

static bool expected_matches(const struct ltfs_info_record *record,
	const struct ltfs_info_expected_medium *expected)
{
	return record->ready && record->media_state == LTFS_INFO_MEDIA_LTFS &&
		record->mam_volume_serial_valid && record->mam_volume_label_valid &&
		record->volume_uuid_valid && record->index_generation_valid &&
		!strcmp(record->identity.drive_serial, expected->drive_serial) &&
		!strcmp(record->mam_volume_serial, expected->medium_serial) &&
		!strcmp(record->mam_volume_label, expected->volume_label) &&
		!strcmp(record->volume_uuid, expected->volume_uuid) &&
		record->index_generation == expected->index_generation;
}

int ltfs_info_collect_capacity(const char *sg_path,
	const struct ltfs_info_identity *identity, struct ltfs_info_backend *backend,
	const struct ltfs_info_expected_medium *expected,
	struct ltfs_info_capacity_report *report)
{
	struct ltfs_info_record before, after;
	struct ltfs_info_capacity capacity = {0};
	int result, close_result;
	if (!report)
		return LTFS_INFO_READ_FAILURE;
	memset(report, 0, sizeof(*report));
	if (!sg_path || !identity || !backend_complete(backend) || !expected ||
		!normalized_identity_text(expected->drive_serial) ||
		!normalized_identity_text(expected->medium_serial) ||
		!normalized_identity_text(expected->volume_label) ||
		!expected->volume_uuid || !info_uuid_valid(expected->volume_uuid) ||
		!expected->index_generation)
		return LTFS_INFO_READ_FAILURE;
	if (strcmp(identity->drive_serial, expected->drive_serial))
		return LTFS_INFO_IDENTITY_MISMATCH;
	result = backend->open(backend->context, sg_path);
	if (result)
		return map_backend_result(result);
	/* One descriptor and the caller's native guard remain held throughout. */
	result = collect_opened(identity, backend, LTFS_INFO_MODE_UNMOUNTED,
		&before, false);
	if (result) {
		result = map_backend_result(result);
		goto close;
	}
	if (!expected_matches(&before, expected)) {
		result = LTFS_INFO_IDENTITY_MISMATCH;
		goto close;
	}
	result = backend->remaining_capacity(backend->context, &capacity);
	if (result) {
		result = map_backend_result(result);
		goto close;
	}
	if ((capacity.log_page != 0x31 && capacity.log_page != 0x17) ||
		capacity.capacity_offset != 0 ||
		capacity.remaining_partition0_mib > capacity.maximum_partition0_mib ||
		capacity.remaining_partition1_mib > capacity.maximum_partition1_mib) {
		result = LTFS_INFO_READ_FAILURE;
		goto close;
	}
	result = collect_opened(identity, backend, LTFS_INFO_MODE_UNMOUNTED,
		&after, false);
	if (result) {
		result = map_backend_result(result);
		goto close;
	}
	if (!expected_matches(&after, expected) ||
		strcmp(before.mam_barcode, after.mam_barcode))
		result = LTFS_INFO_IDENTITY_MISMATCH;
close:
	close_result = backend->close(backend->context);
	if (!result && close_result)
		result = map_backend_result(close_result);
	/* No successful report, including partial capacity, survives any failure. */
	if (!result) {
		report->record = before;
		report->record.capacity = capacity;
		report->record.capacity_valid = true;
		report->identity_verified_before_after = true;
	}
	return result;
}

int ltfs_info_collect(const char *sg_path,
	const struct ltfs_info_identity *identity,
	struct ltfs_info_backend *backend, struct ltfs_info_record *record)
{
	return ltfs_info_collect_mode(sg_path, identity, backend,
		LTFS_INFO_MODE_UNMOUNTED, record);
}

static int json_string(FILE *stream, const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	if (fputc('"', stream) == EOF)
		return -EIO;
	for (; *cursor; ++cursor) {
		if (*cursor == '"' || *cursor == '\\') {
			if (fputc('\\', stream) == EOF || fputc(*cursor, stream) == EOF)
				return -EIO;
		} else if (*cursor == '\n') {
			if (fputs("\\n", stream) == EOF)
				return -EIO;
		} else if (*cursor == '\r') {
			if (fputs("\\r", stream) == EOF)
				return -EIO;
		} else if (*cursor == '\t') {
			if (fputs("\\t", stream) == EOF)
				return -EIO;
		} else if (*cursor < 0x20) {
			if (fprintf(stream, "\\u%04x", *cursor) < 0)
				return -EIO;
		} else if (fputc(*cursor, stream) == EOF)
			return -EIO;
	}
	return fputc('"', stream) == EOF ? -EIO : 0;
}

static int json_optional_string(FILE *stream, bool valid, const char *value)
{
	return valid ? json_string(stream, value) :
		(fputs("null", stream) == EOF ? -EIO : 0);
}

static bool config_text(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	if (!value || !*value)
		return false;
	for (; *cursor; ++cursor)
		if (*cursor < 0x21 || *cursor > 0x7e || *cursor == '"' ||
			*cursor == '\\')
			return false;
	return true;
}

static bool stable_config_path(const char *path, const char *prefix)
{
	const char *name;
	if (!path || strncmp(path, prefix, strlen(prefix)))
		return false;
	name = path + strlen(prefix);
	return config_text(name) && !strchr(name, '/');
}

int ltfs_info_set_config_paths(struct ltfs_info_record *record,
	const char *tape_by_id, const char *scsi_by_id)
{
	if (!record ||
		!stable_config_path(tape_by_id, "/dev/tape/by-id/") ||
		!stable_config_path(scsi_by_id, "/dev/lto-archiver-scsi-") ||
		copy_text(record->tape_by_id, sizeof(record->tape_by_id),
			tape_by_id) < 0 ||
		copy_text(record->scsi_by_id, sizeof(record->scsi_by_id),
			scsi_by_id) < 0)
		return -EINVAL;
	return 0;
}

int ltfs_info_write_device_config_json(FILE *stream, const char *nst_path,
	const char *sg_path, const struct ltfs_info_identity *identity)
{
	if (!stream || !identity ||
		!stable_config_path(nst_path, "/dev/tape/by-id/") ||
		!stable_config_path(sg_path, "/dev/lto-archiver-scsi-") ||
		!config_text(identity->drive_serial) ||
		!config_text(identity->drive_wwid))
		return -EINVAL;
	if (fprintf(stream, "{\"nst_path\":") < 0 ||
		json_string(stream, nst_path) < 0 ||
		fprintf(stream, ",\"sg_path\":") < 0 ||
		json_string(stream, sg_path) < 0 ||
		fprintf(stream, ",\"serial\":") < 0 ||
		json_string(stream, identity->drive_serial) < 0 ||
		fprintf(stream, ",\"wwid\":") < 0 ||
		json_string(stream, identity->drive_wwid) < 0 ||
		fprintf(stream, "}\n") < 0)
		return -EIO;
	return 0;
}

bool ltfs_info_opcode_allowed(uint8_t opcode)
{
	return opcode == SCSI_TEST_UNIT_READY || opcode == SCSI_INQUIRY ||
		opcode == SCSI_LOG_SENSE || opcode == SCSI_READ_ATTRIBUTE;
}

static uint16_t info_be16(const unsigned char *value)
{
	return ((uint16_t)value[0] << 8) | value[1];
}

static uint32_t info_be32(const unsigned char *value)
{
	return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
		((uint32_t)value[2] << 8) | value[3];
}

static uint64_t info_be64(const unsigned char *value)
{
	return ((uint64_t)info_be32(value) << 32) | info_be32(value + 4);
}

int ltfs_info_extract_attribute_response(const unsigned char *response,
	size_t response_size, uint16_t attribute, unsigned char *output,
	size_t output_size)
{
	size_t available;
	size_t descriptor_size;
	if (!response || response_size < 9 || !output || output_size < 5)
		return LTFS_INFO_BACKEND_READ_ERROR;
	available = info_be32(response);
	if (available < 5 || info_be16(response + 4) != attribute)
		return LTFS_INFO_BACKEND_READ_ERROR;
	descriptor_size = (size_t)info_be16(response + 7) + 5;
	if (descriptor_size > available || descriptor_size > response_size - 4 ||
		descriptor_size > output_size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	memcpy(output, response + 4, descriptor_size);
	return 0;
}

int ltfs_info_parse_text_attribute_descriptor(const unsigned char *descriptor,
	size_t descriptor_size, uint16_t attribute, char *output,
	size_t output_size)
{
	size_t length;
	if (!descriptor || descriptor_size < 5 || !output || !output_size ||
		info_be16(descriptor) != attribute)
		return LTFS_INFO_BACKEND_READ_ERROR;
	length = info_be16(descriptor + 3);
	if (!length || length > descriptor_size - 5)
		return LTFS_INFO_BACKEND_READ_ERROR;
	return ltfs_device_identity_normalize_text(output, output_size,
		descriptor + 5, length) == 0 ? 0 : LTFS_INFO_BACKEND_UNSUPPORTED;
}

int ltfs_info_build_capacity_log_sense_cdb(unsigned char cdb[10],
	size_t allocation_size)
{
	if (!cdb || allocation_size == 0 || allocation_size > UINT16_MAX)
		return LTFS_INFO_BACKEND_READ_ERROR;
	memset(cdb, 0, 10);
	cdb[0] = SCSI_LOG_SENSE;
	cdb[2] = 0x40 | 0x17;
	cdb[7] = (unsigned char)(allocation_size >> 8);
	cdb[8] = (unsigned char)allocation_size;
	return 0;
}

static bool info_uuid_valid(const char *uuid)
{
	size_t index;
	if (!uuid || strlen(uuid) != 36)
		return false;
	for (index = 0; index < 36; ++index) {
		unsigned char byte = (unsigned char)uuid[index];
		if (index == 8 || index == 13 || index == 18 || index == 23) {
			if (byte != '-')
				return false;
		} else if (!((byte >= '0' && byte <= '9') ||
			(byte >= 'a' && byte <= 'f') ||
			(byte >= 'A' && byte <= 'F')))
			return false;
	}
	return true;
}

int ltfs_info_parse_coherency(const unsigned char *raw, size_t size,
	char uuid[LTFS_INFO_UUID_SIZE], uint64_t *generation)
{
	if (!raw || size != LTFS_INFO_COHERENCY_SIZE || !uuid || !generation ||
		info_be16(raw) != LTFS_INFO_COHERENCY ||
		info_be16(raw + 3) != 0x46 || raw[5] != 8 ||
		(info_be16(raw + 30) != 42 && info_be16(raw + 30) != 43) ||
		memcmp(raw + 32, "LTFS", 4))
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	memcpy(uuid, raw + 37, 36);
	uuid[36] = '\0';
	if (!info_uuid_valid(uuid))
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	*generation = info_be64(raw + 14);
	return 0;
}

static int info_log_parameter(const unsigned char *page, size_t page_size,
	uint16_t wanted, const unsigned char **value, size_t *value_size)
{
	size_t total;
	size_t offset = 4;
	if (!page || page_size < 4 || !value || !value_size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	total = (size_t)info_be16(page + 2) + 4;
	if (total > page_size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	while (offset + 4 <= total) {
		size_t length = page[offset + 3];
		if (offset + 4 + length > total)
			return LTFS_INFO_BACKEND_READ_ERROR;
		if (info_be16(page + offset) == wanted) {
			*value = page + offset + 4;
			*value_size = length;
			return 0;
		}
		offset += 4 + length;
	}
	return LTFS_INFO_BACKEND_UNSUPPORTED;
}

static int info_partition_capacities(const unsigned char *value, size_t size,
	uint64_t *partition0, uint64_t *partition1)
{
	size_t offset;
	size_t length;
	if (!value || size < 8 || !partition0 || !partition1)
		return LTFS_INFO_BACKEND_READ_ERROR;
	length = (size_t)value[0] + 1;
	if (length < 8 || length > size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	*partition0 = info_be32(value + 4);
	offset = length;
	if (offset + 8 > size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	length = (size_t)value[offset] + 1;
	if (length < 8 || offset + length > size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	*partition1 = info_be32(value + offset + 4);
	return 0;
}

int ltfs_info_parse_capacity_page17(const unsigned char *page, size_t size,
	struct ltfs_info_capacity *capacity)
{
	const unsigned char *maximum;
	const unsigned char *remaining;
	size_t maximum_size;
	size_t remaining_size;
	bool remaining_is_used = false;
	int result;
	if (!capacity)
		return LTFS_INFO_BACKEND_READ_ERROR;
	memset(capacity, 0, sizeof(*capacity));
	result = info_log_parameter(page, size, 0x0202,
		&maximum, &maximum_size);
	if (result)
		return result;
	result = info_log_parameter(page, size, 0x0204,
		&remaining, &remaining_size);
	if (result == LTFS_INFO_BACKEND_UNSUPPORTED) {
		result = info_log_parameter(page, size, 0x0203,
			&remaining, &remaining_size);
		remaining_is_used = result == 0;
	}
	if (result)
		return result;
	result = info_partition_capacities(maximum, maximum_size,
		&capacity->maximum_partition0_mib,
		&capacity->maximum_partition1_mib);
	if (result)
		return result;
	result = info_partition_capacities(remaining, remaining_size,
		&capacity->remaining_partition0_mib,
		&capacity->remaining_partition1_mib);
	if (result)
		return result;
	if (remaining_is_used) {
		if (capacity->remaining_partition0_mib == UINT32_MAX ||
			capacity->remaining_partition1_mib == UINT32_MAX)
			return LTFS_INFO_BACKEND_UNSUPPORTED;
		if (capacity->remaining_partition0_mib >
				capacity->maximum_partition0_mib ||
			capacity->remaining_partition1_mib >
				capacity->maximum_partition1_mib)
			return LTFS_INFO_BACKEND_READ_ERROR;
		capacity->remaining_partition0_mib =
			capacity->maximum_partition0_mib -
			capacity->remaining_partition0_mib;
		capacity->remaining_partition1_mib =
			capacity->maximum_partition1_mib -
			capacity->remaining_partition1_mib;
	}
	capacity->maximum_partition0_mib =
		(capacity->maximum_partition0_mib * 1000000ULL) >> 20;
	capacity->maximum_partition1_mib =
		(capacity->maximum_partition1_mib * 1000000ULL) >> 20;
	capacity->remaining_partition0_mib =
		(capacity->remaining_partition0_mib * 1000000ULL) >> 20;
	capacity->remaining_partition1_mib =
		(capacity->remaining_partition1_mib * 1000000ULL) >> 20;
	return 0;
}

int ltfs_info_write_json(FILE *stream, const struct ltfs_info_record *record)
{
	const char *media_state;
	if (!stream || !record ||
		!stable_config_path(record->tape_by_id, "/dev/tape/by-id/") ||
		!stable_config_path(record->scsi_by_id,
			"/dev/lto-archiver-scsi-"))
		return -EINVAL;
	switch (record->media_state) {
	case LTFS_INFO_MEDIA_LTFS:
		media_state = "ltfs";
		break;
	case LTFS_INFO_MEDIA_UNIDENTIFIED:
		media_state = "unidentified";
		break;
	default:
		return -EINVAL;
	}
	if (fputs("{\"schema\":2,\"media_state\":", stream) == EOF ||
		json_string(stream, media_state) < 0 ||
		fputs(",\"tape_by_id\":", stream) == EOF ||
		json_string(stream, record->tape_by_id) < 0 ||
		fputs(",\"scsi_by_id\":", stream) == EOF ||
		json_string(stream, record->scsi_by_id) < 0 ||
		fputs(",\"drive_serial\":", stream) == EOF ||
		json_string(stream, record->identity.drive_serial) < 0 ||
		fputs(",\"mam_barcode\":", stream) == EOF ||
		json_optional_string(stream, record->mam_barcode_valid,
			record->mam_barcode) < 0 ||
		fputs(",\"mam_volume_serial\":", stream) == EOF ||
		json_optional_string(stream, record->mam_volume_serial_valid,
			record->mam_volume_serial) < 0 ||
		fputs(",\"ltfs_volume_label\":", stream) == EOF ||
		json_optional_string(stream, record->mam_volume_label_valid,
			record->mam_volume_label) < 0 ||
		fputs(",\"ltfs_volume_uuid\":", stream) == EOF ||
		json_optional_string(stream, record->volume_uuid_valid,
			record->volume_uuid) < 0 ||
		fputs(",\"index_generation\":", stream) == EOF)
		return -EIO;
	if (record->index_generation_valid) {
		if (fprintf(stream, "%" PRIu64, record->index_generation) < 0)
			return -EIO;
	} else if (fputs("null", stream) == EOF)
		return -EIO;
	return fputs("}\n", stream) == EOF ? -EIO : 0;
}

int ltfs_info_write_capacity_json(FILE *stream,
	const struct ltfs_info_capacity_report *report)
{
	const struct ltfs_info_capacity *capacity;
	if (!stream || !report || !report->identity_verified_before_after ||
		!report->record.capacity_valid)
		return -EINVAL;
	capacity = &report->record.capacity;
	if (fprintf(stream, "{\"schema\":1,\"kind\":\"ltfs-capacity\","
		"\"identity_verified_before_after\":true,\"unit\":\"MiB\","
		"\"capacity_offset\":%u,\"log_page\":%u,"
		"\"remaining_partition0_mib\":%" PRIu64 ","
		"\"remaining_partition1_mib\":%" PRIu64 ","
		"\"maximum_partition0_mib\":%" PRIu64 ","
		"\"maximum_partition1_mib\":%" PRIu64 ",\"identity\":",
		capacity->capacity_offset, capacity->log_page,
		capacity->remaining_partition0_mib, capacity->remaining_partition1_mib,
		capacity->maximum_partition0_mib, capacity->maximum_partition1_mib) < 0 ||
		ltfs_info_write_json(stream, &report->record) < 0 ||
		fputs("}\n", stream) == EOF)
		return -EIO;
	return 0;
}

struct self_test_backend {
	bool opened;
	bool pre_format;
};

static int self_test_open(void *context, const char *path)
{
	struct self_test_backend *self_test = context;
	if (!self_test || !path || strcmp(path, "/fixture/sg"))
		return LTFS_INFO_BACKEND_READ_ERROR;
	self_test->opened = true;
	return 0;
}

static int self_test_close(void *context)
{
	struct self_test_backend *self_test = context;
	if (!self_test || !self_test->opened)
		return LTFS_INFO_BACKEND_READ_ERROR;
	self_test->opened = false;
	return 0;
}

static int self_test_ready(void *context)
{
	struct self_test_backend *self_test = context;
	return self_test && self_test->opened ? 0 : LTFS_INFO_BACKEND_READ_ERROR;
}

static int self_test_text(void *context, uint16_t attribute, char *value,
	size_t size)
{
	struct self_test_backend *self_test = context;
	const char *text;
	if (self_test_ready(context))
		return LTFS_INFO_BACKEND_READ_ERROR;
	if (self_test->pre_format && attribute == LTFS_INFO_APP_NAME)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	if (attribute == LTFS_INFO_BARCODE)
		text = "SELFTEST01";
	else if (attribute == TC_MAM_MEDIUM_SERIAL_NUMBER)
		text = "SELFTEST-SERIAL-01";
	else if (attribute == LTFS_INFO_APP_NAME)
		text = "LTFS";
	else if (attribute == LTFS_INFO_APP_VERSION)
		text = "self-test";
	else if (attribute == LTFS_INFO_VOLUME_LABEL)
		text = "SELF TEST";
	else
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	return copy_text(value, size, text) < 0 ?
		LTFS_INFO_BACKEND_READ_ERROR : 0;
}

static int self_test_coherency(void *context,
	char uuid[LTFS_INFO_UUID_SIZE], uint64_t *generation)
{
	if (self_test_ready(context) || !uuid || !generation ||
		copy_text(uuid, LTFS_INFO_UUID_SIZE,
			"11111111-2222-3333-4444-555555555555") < 0)
		return LTFS_INFO_BACKEND_READ_ERROR;
	*generation = 1;
	return 0;
}

static int self_test_capacity(void *context,
	struct ltfs_info_capacity *capacity)
{
	if (self_test_ready(context) || !capacity)
		return LTFS_INFO_BACKEND_READ_ERROR;
	capacity->remaining_partition0_mib = 1;
	capacity->log_page = 0x31;
	capacity->capacity_offset = 0;
	capacity->remaining_partition1_mib = 2;
	capacity->maximum_partition0_mib = 3;
	capacity->maximum_partition1_mib = 4;
	return 0;
}

int ltfs_info_run_self_test_fixture(FILE *stream, const char *name)
{
	struct self_test_backend context = {0};
	struct ltfs_info_identity identity = {0};
	struct ltfs_info_backend backend = {
		.read_only_guaranteed = true,
		.context = &context,
		.open = self_test_open,
		.close = self_test_close,
		.inquiry = self_test_ready,
		.test_unit_ready = self_test_ready,
		.read_text_attribute = self_test_text,
		.read_coherency = self_test_coherency,
		.remaining_capacity = self_test_capacity,
	};
	struct ltfs_info_record record;
	struct ltfs_info_capacity_report report;
	const struct ltfs_info_expected_medium expected = {
		.drive_serial = "SELFTEST", .medium_serial = "SELFTEST-SERIAL-01",
		.volume_label = "SELF TEST",
		.volume_uuid = "11111111-2222-3333-4444-555555555555",
		.index_generation = 1,
	};
	enum ltfs_info_mode mode;
	int result;
	if (!stream || !name || (strcmp(name, "ready") &&
		strcmp(name, "pre-format") && strcmp(name, "capacity")))
		return 2;
	context.pre_format = !strcmp(name, "pre-format");
	mode = context.pre_format ? LTFS_INFO_MODE_PRE_FORMAT :
		LTFS_INFO_MODE_UNMOUNTED;
	memset(identity.target_sha256, 'a',
		sizeof(identity.target_sha256) - 1);
	if (copy_text(identity.drive_serial, sizeof(identity.drive_serial),
		"SELFTEST") < 0 ||
		copy_text(identity.drive_wwid, sizeof(identity.drive_wwid),
			"naa.selftest") < 0 ||
		copy_text(identity.scsi_tuple, sizeof(identity.scsi_tuple),
			"self:test") < 0)
		return LTFS_INFO_READ_FAILURE;
	if (!strcmp(name, "capacity")) {
		result = ltfs_info_collect_capacity("/fixture/sg", &identity, &backend,
			&expected, &report);
		record = report.record;
	} else
		result = ltfs_info_collect_mode("/fixture/sg", &identity, &backend,
			mode, &record);
	if (result == LTFS_INFO_READY && ltfs_info_set_config_paths(&record,
		"/dev/tape/by-id/self-test-nst",
		"/dev/lto-archiver-scsi-self-test-sg") < 0)
		return LTFS_INFO_READ_FAILURE;
	if (result == LTFS_INFO_READY) {
		if (!strcmp(name, "capacity")) {
			report.record = record;
			result = ltfs_info_write_capacity_json(stream, &report);
		} else
			result = ltfs_info_write_json(stream, &record);
		if (result < 0)
			return LTFS_INFO_READ_FAILURE;
	}
	return result;
}

#if defined(__linux__) && !defined(LTFS_INFO_NO_MAIN)
static int sense_result(const unsigned char *sense, size_t length,
	unsigned char status)
{
	unsigned char key = 0;
	unsigned char asc = 0;
	if (status == 0x18)
		return LTFS_INFO_BACKEND_BUSY;
	if (length >= 14 && (sense[0] & 0x7f) == 0x70) {
		key = sense[2] & 0x0f;
		asc = sense[12];
	} else if (length >= 4 && (sense[0] & 0x7f) == 0x72) {
		key = sense[1] & 0x0f;
		asc = sense[2];
	}
	if (key == 0x02 && asc == 0x3a)
		return LTFS_INFO_BACKEND_NO_MEDIA;
	if (key == 0x05)
		return LTFS_INFO_BACKEND_UNSUPPORTED;
	return LTFS_INFO_BACKEND_READ_ERROR;
}

static int linux_sg_command(struct linux_sg_backend *backend,
	const unsigned char *cdb, size_t cdb_size, void *data, size_t data_size)
{
	sg_io_hdr_t request;
	unsigned char sense[64] = {0};
	int result;
	if (!backend || backend->fd < 0 || !cdb || !cdb_size ||
		!ltfs_info_opcode_allowed(cdb[0]) || cdb_size > UINT8_MAX ||
		data_size > UINT32_MAX)
		return LTFS_INFO_BACKEND_READ_ERROR;
	memset(&request, 0, sizeof(request));
	request.interface_id = 'S';
	request.dxfer_direction = data_size ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
	request.cmd_len = (unsigned char)cdb_size;
	request.mx_sb_len = sizeof(sense);
	request.dxfer_len = (unsigned int)data_size;
	request.dxferp = data;
	request.cmdp = (unsigned char *)cdb;
	request.sbp = sense;
	request.timeout = LTFS_INFO_TIMEOUT_MS;
	do {
		result = ioctl(backend->fd, SG_IO, &request);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		return errno == EBUSY || errno == EAGAIN ?
			LTFS_INFO_BACKEND_BUSY : LTFS_INFO_BACKEND_READ_ERROR;
	if ((request.info & SG_INFO_OK_MASK) != SG_INFO_OK)
		return sense_result(sense, request.sb_len_wr, request.status);
	if (request.host_status || request.driver_status || request.resid < 0 ||
		(size_t)request.resid > data_size)
		return LTFS_INFO_BACKEND_READ_ERROR;
	backend->transferred = data_size - (size_t)request.resid;
	return 0;
}

static int linux_open(void *context, const char *path)
{
	struct linux_sg_backend *backend = context;
	struct stat status;
	if (!backend || backend->fd >= 0 || !path)
		return LTFS_INFO_BACKEND_READ_ERROR;
	backend->fd = open(path,
		O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
	if (backend->fd < 0)
		return errno == EBUSY || errno == EAGAIN ?
			LTFS_INFO_BACKEND_BUSY : LTFS_INFO_BACKEND_READ_ERROR;
	if (fstat(backend->fd, &status) < 0 || !S_ISCHR(status.st_mode) ||
		status.st_rdev != backend->expected_rdev) {
		close(backend->fd);
		backend->fd = -1;
		return LTFS_INFO_BACKEND_READ_ERROR;
	}
	return 0;
}

static int linux_close(void *context)
{
	struct linux_sg_backend *backend = context;
	int fd;
	int result;
	if (!backend || backend->fd < 0)
		return LTFS_INFO_BACKEND_READ_ERROR;
	fd = backend->fd;
	backend->fd = -1;
	result = close(fd);
	return result < 0 ? LTFS_INFO_BACKEND_READ_ERROR : 0;
}

static int linux_inquiry(void *context)
{
	struct linux_sg_backend *backend = context;
	unsigned char cdb[6] = {SCSI_INQUIRY, 0, 0, 0, 96, 0};
	unsigned char data[96] = {0};
	unsigned char page = 0;
	unsigned char serial_cdb[6] = {SCSI_INQUIRY, 1, 0x80, 0, 255, 0};
	unsigned char serial_data[255] = {0};
	char serial[LTFS_INFO_TEXT_MAX];
	int result = linux_sg_command(context, cdb, sizeof(cdb), data, sizeof(data));
	if (result || !backend->capacity_diagnostic)
		return result;
	result = ltfs_info_select_capacity_page(data, backend->transferred, &page);
	if (result)
		return result;
	if (backend->capacity_page && backend->capacity_page != page)
		return LTFS_INFO_BACKEND_READ_ERROR;
	backend->capacity_page = page;
	result = linux_sg_command(context, serial_cdb, sizeof(serial_cdb),
		serial_data, sizeof(serial_data));
	if (result)
		return result;
	if (backend->transferred < 4 || serial_data[0] != 1 || serial_data[1] != 0x80 ||
		(size_t)info_be16(serial_data + 2) > backend->transferred - 4 ||
		ltfs_device_identity_normalize_text(serial, sizeof(serial), serial_data + 4,
			info_be16(serial_data + 2)) || !backend->expected_serial ||
		strcmp(serial, backend->expected_serial))
		return LTFS_INFO_BACKEND_READ_ERROR;
	return 0;
}

static int linux_ready(void *context)
{
	unsigned char cdb[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
	return linux_sg_command(context, cdb, sizeof(cdb), NULL, 0);
}

static int linux_read_attribute_raw(void *context, unsigned char partition,
	uint16_t attribute, unsigned char *output, size_t output_size)
{
	unsigned char cdb[16] = {0};
	unsigned char *buffer;
	size_t transfer_size;
	int result;
	if (!output || output_size < 5 || output_size > UINT32_MAX - 4)
		return LTFS_INFO_BACKEND_READ_ERROR;
	transfer_size = output_size + 4;
	buffer = calloc(1, transfer_size);
	if (!buffer)
		return LTFS_INFO_BACKEND_READ_ERROR;
	cdb[0] = SCSI_READ_ATTRIBUTE;
	cdb[7] = partition;
	cdb[8] = (unsigned char)(attribute >> 8);
	cdb[9] = (unsigned char)attribute;
	cdb[10] = (unsigned char)(transfer_size >> 24);
	cdb[11] = (unsigned char)(transfer_size >> 16);
	cdb[12] = (unsigned char)(transfer_size >> 8);
	cdb[13] = (unsigned char)transfer_size;
	result = linux_sg_command(context, cdb, sizeof(cdb), buffer,
		transfer_size);
	if (result)
		goto out;
	result = ltfs_info_extract_attribute_response(buffer,
		((struct linux_sg_backend *)context)->transferred,
		attribute, output, output_size);
out:
	free(buffer);
	return result;
}

static int linux_text(void *context, uint16_t attribute, char *value,
	size_t size)
{
	unsigned char raw[5 + LTFS_INFO_TEXT_MAX] = {0};
	int result = linux_read_attribute_raw(context, 0, attribute, raw,
		sizeof(raw));
	if (result)
		return result;
	return ltfs_info_parse_text_attribute_descriptor(raw, sizeof(raw),
		attribute, value, size);
}

static int linux_coherency(void *context,
	char uuid[LTFS_INFO_UUID_SIZE], uint64_t *generation)
{
	unsigned char raw[LTFS_INFO_COHERENCY_SIZE] = {0};
	int result = linux_read_attribute_raw(context, 0, LTFS_INFO_COHERENCY,
		raw, sizeof(raw));
	if (result)
		return result;
	return ltfs_info_parse_coherency(raw, sizeof(raw), uuid, generation);
}

static int linux_capacity(void *context,
	struct ltfs_info_capacity *capacity)
{
	struct linux_sg_backend *backend = context;
	struct tc_remaining_cap parsed = {0};
	unsigned char cdb[10];
	unsigned char page[1024] = {0};
	size_t page_size;
	int result;
	result = ltfs_info_build_capacity_log_sense_cdb(cdb, sizeof(page));
	if (result)
		return result;
	if (backend->capacity_diagnostic) {
		if (backend->capacity_page != 0x17 && backend->capacity_page != 0x31)
			return LTFS_INFO_BACKEND_UNSUPPORTED;
		cdb[2] = 0x40 | backend->capacity_page;
	}
	result = linux_sg_command(context, cdb, sizeof(cdb), page,
		sizeof(page));
	if (result)
		return result;
	if (!backend->capacity_diagnostic)
		return ltfs_info_parse_capacity_page17(page, backend->transferred, capacity);
	if (backend->transferred < 4)
		return LTFS_INFO_BACKEND_READ_ERROR;
	page_size = (size_t)info_be16(page + 2) + 4;
	if (page_size > backend->transferred)
		return LTFS_INFO_BACKEND_READ_ERROR;
	/* Diagnostic reports raw drive capacity at explicit offset zero. It does
	 * not claim this is the writable filesystem payload or apply a reserve. */
	result = backend->capacity_page == 0x31 ?
		sg_parse_capacity_page31(page, page_size, 0, &parsed) :
		sg_parse_capacity_page17(page, page_size, 0, &parsed);
	if (result)
		return LTFS_INFO_BACKEND_READ_ERROR;
	memset(capacity, 0, sizeof(*capacity));
	capacity->log_page = backend->capacity_page;
	capacity->remaining_partition0_mib = parsed.remaining_p0;
	capacity->remaining_partition1_mib = parsed.remaining_p1;
	capacity->maximum_partition0_mib = parsed.max_p0;
	capacity->maximum_partition1_mib = parsed.max_p1;
	return 0;
}
#endif /* __linux__ && !LTFS_INFO_NO_MAIN */

#ifndef LTFS_INFO_NO_MAIN
static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s --version | --self-test-fixture ready|pre-format|capacity | "
		"--diagnose-identity | "
		"--json --mode unmounted|pre-format | "
		"--json --mode capacity --expect-drive-serial SERIAL "
		"--expect-medium-serial SERIAL --expect-label LABEL "
		"--expect-uuid UUID --expect-generation N | "
		"--discover --json --device <stable-sg> "
		"--tape-device <stable-nst>\n",
		program);
}

int main(int argc, char **argv)
{
#ifdef __linux__
	const char *sg_path = NULL;
	const char *nst_path = NULL;
	const char *mode = NULL;
	const char *generation_text = NULL;
	struct ltfs_info_expected_medium expected = {0};
	struct ltfs_info_capacity_report report;
	bool capacity_mode = false;
	bool json = false;
	bool discover = false;
	struct ltfs_device_target target;
	struct ltfs_device_identity resolved;
	struct ltfs_device_config config;
	struct ltfs_device_guard_roots roots;
	struct ltfs_device_guard guard;
	struct ltfs_info_identity identity = {0};
	struct linux_sg_backend linux_context = {.fd = -1, .expected_rdev = 0};
	struct ltfs_info_backend backend = {
		.read_only_guaranteed = true,
		.context = &linux_context,
		.open = linux_open,
		.close = linux_close,
		.inquiry = linux_inquiry,
		.test_unit_ready = linux_ready,
		.read_text_attribute = linux_text,
		.read_coherency = linux_coherency,
		.remaining_capacity = linux_capacity,
	};
	struct ltfs_info_record record;
	enum ltfs_info_mode requested_mode = LTFS_INFO_MODE_UNMOUNTED;
	int index;
	int result;
	if (argc == 2 && !strcmp(argv[1], "--version")) {
		printf("ltfs-info %s\n", PACKAGE_VERSION);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "--self-test-fixture"))
		return ltfs_info_run_self_test_fixture(stdout, argv[2]);
	if (argc == 2 && !strcmp(argv[1], "--diagnose-identity")) {
		struct ltfs_device_identity_diagnostic diagnostic = {
			.phase = LTFS_DEVICE_IDENTITY_PHASE_ARGUMENTS,
			.error = 0,
		};
		const char *phase = "config-load";

		result = ltfs_device_config_load(LTFS_DEVICE_CONFIG_PATH, &config);
		if (result == 0) {
			memset(&target, 0, sizeof(target));
			target.nst_path = config.nst_path;
			target.sg_path = config.sg_path;
			target.expected_serial = config.expected_serial;
			target.expected_wwid = config.expected_wwid;
			target.device_root = "/dev";
			target.sysfs_root = "/sys";
			target.proc_root = "/proc";
			target.lock_root = "/run/lock/lto-ltfs";
			result = ltfs_device_identity_resolve_diagnostic(&target,
				&resolved, &diagnostic);
			phase = ltfs_device_identity_phase_name(diagnostic.phase);
		}
		if (printf("{\"schema\":1,\"phase\":\"%s\",\"error\":%d}\n",
			phase, result < 0 ? -result : 0) < 0)
			return LTFS_INFO_READ_FAILURE;
		return result < 0 ? LTFS_INFO_IDENTITY_MISMATCH : LTFS_INFO_READY;
	}
	for (index = 1; index < argc; ++index) {
		if (!strcmp(argv[index], "--json"))
			json = true;
		else if (!strcmp(argv[index], "--discover"))
			discover = true;
		else if (!strcmp(argv[index], "--mode") && index + 1 < argc)
			mode = argv[++index];
		else if (!strcmp(argv[index], "--device") && index + 1 < argc)
			sg_path = argv[++index];
		else if (!strcmp(argv[index], "--tape-device") && index + 1 < argc)
			nst_path = argv[++index];
		else if (!strcmp(argv[index], "--expect-drive-serial") && index + 1 < argc && !expected.drive_serial)
			expected.drive_serial = argv[++index];
		else if (!strcmp(argv[index], "--expect-medium-serial") && index + 1 < argc && !expected.medium_serial)
			expected.medium_serial = argv[++index];
		else if (!strcmp(argv[index], "--expect-label") && index + 1 < argc && !expected.volume_label)
			expected.volume_label = argv[++index];
		else if (!strcmp(argv[index], "--expect-uuid") && index + 1 < argc && !expected.volume_uuid)
			expected.volume_uuid = argv[++index];
		else if (!strcmp(argv[index], "--expect-generation") && index + 1 < argc && !generation_text)
			generation_text = argv[++index];
		else {
			usage(argv[0]);
			return 2;
		}
	}
	if (!json || (discover && (!sg_path || !nst_path || mode)) ||
		(!discover && (sg_path || nst_path || !mode)) ||
		(mode && strcmp(mode, "unmounted") && strcmp(mode, "pre-format") &&
			strcmp(mode, "mounted") && strcmp(mode, "capacity"))) {
		usage(argv[0]);
		return 2;
	}
	capacity_mode = mode && !strcmp(mode, "capacity");
	if (capacity_mode) {
		char *end;
		if (!normalized_identity_text(expected.drive_serial) ||
			!normalized_identity_text(expected.medium_serial) ||
			!normalized_identity_text(expected.volume_label) ||
			!expected.volume_uuid || !info_uuid_valid(expected.volume_uuid) ||
			!generation_text || !*generation_text)
			return 2;
		for (const char *p = generation_text; *p; ++p)
			if (*p < '0' || *p > '9') return 2;
		errno = 0;
		expected.index_generation = strtoull(generation_text, &end, 10);
		if (errno || *end || !expected.index_generation)
			return 2;
	} else if (expected.drive_serial || expected.medium_serial || expected.volume_label ||
		expected.volume_uuid || generation_text)
		return 2;
	if (mode && !strcmp(mode, "mounted"))
		return LTFS_INFO_DEVICE_BUSY;
	if (mode && !strcmp(mode, "pre-format"))
		requested_mode = LTFS_INFO_MODE_PRE_FORMAT;
	if (discover) {
		struct ltfs_info_identity discovered = {0};
		if (!stable_config_path(nst_path, "/dev/tape/by-id/") ||
			!stable_config_path(sg_path, "/dev/lto-archiver-scsi-"))
			return LTFS_INFO_IDENTITY_MISMATCH;
		memset(&target, 0, sizeof(target));
		target.nst_path = nst_path;
		target.sg_path = sg_path;
		target.device_root = "/dev";
		target.sysfs_root = "/sys";
		target.proc_root = "/proc";
		target.lock_root = "/run/lock/lto-ltfs";
		result = ltfs_device_identity_resolve(&target, &resolved);
		if (result < 0 ||
			copy_text(discovered.drive_serial,
				sizeof(discovered.drive_serial), resolved.serial) < 0 ||
			copy_text(discovered.drive_wwid,
				sizeof(discovered.drive_wwid), resolved.wwid) < 0)
			return LTFS_INFO_IDENTITY_MISMATCH;
		return ltfs_info_write_device_config_json(stdout, nst_path,
			sg_path, &discovered) < 0 ? LTFS_INFO_IDENTITY_MISMATCH : 0;
	}
	result = ltfs_device_config_load(LTFS_DEVICE_CONFIG_PATH, &config);
	if (result < 0)
		return LTFS_INFO_IDENTITY_MISMATCH;
	memset(&roots, 0, sizeof(roots));
	roots.device_root = "/dev";
	roots.sysfs_root = "/sys";
	roots.proc_root = "/proc";
	roots.lock_root = "/run/lock/lto-ltfs";
	result = ltfs_device_guard_acquire(&config, config.sg_path, &roots,
		"00000000-0000-4000-8000-000000000001",
		LTFS_COMMAND_CLASS_READ_ONLY_INFO, &guard);
	if (result == LTFS_OPERATION_LOCK_BUSY)
		return LTFS_INFO_DEVICE_BUSY;
	if (result < 0)
		return LTFS_INFO_IDENTITY_MISMATCH;
	resolved = guard.identity;
	if (copy_text(identity.target_sha256, sizeof(identity.target_sha256),
		resolved.target_sha256) < 0 ||
		copy_text(identity.drive_serial, sizeof(identity.drive_serial),
			resolved.serial) < 0 ||
		copy_text(identity.drive_wwid, sizeof(identity.drive_wwid),
			resolved.wwid) < 0 ||
		copy_text(identity.scsi_tuple, sizeof(identity.scsi_tuple),
			resolved.scsi_tuple) < 0) {
		result = LTFS_INFO_IDENTITY_MISMATCH;
		goto out_guard;
	}
	linux_context.expected_rdev = resolved.sg_rdev;
	linux_context.capacity_diagnostic = capacity_mode;
	linux_context.expected_serial = expected.drive_serial;
	if (capacity_mode) {
		result = ltfs_info_collect_capacity(resolved.sg_path, &identity, &backend,
			&expected, &report);
		record = report.record;
	} else
		result = ltfs_info_collect_mode(resolved.sg_path, &identity, &backend,
			requested_mode, &record);
	if (result == LTFS_INFO_READY && ltfs_info_set_config_paths(&record,
		config.nst_path, config.sg_path) < 0)
		result = LTFS_INFO_IDENTITY_MISMATCH;
	if (result == LTFS_INFO_READY) {
		if (capacity_mode) {
			report.record = record;
			result = ltfs_info_write_capacity_json(stdout, &report);
		} else
			result = ltfs_info_write_json(stdout, &record);
		if (result < 0)
			result = LTFS_INFO_READ_FAILURE;
	}
out_guard:
	ltfs_device_guard_release(&guard);
	return result;
#else
	(void)argc;
	(void)argv;
	return LTFS_INFO_READ_FAILURE;
#endif
}
#endif /* LTFS_INFO_NO_MAIN */
