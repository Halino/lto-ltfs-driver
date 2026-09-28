/* SPDX-License-Identifier: LGPL-2.1-only */
/* Downstream modifications: 2026-08 through 2026-09; see LGPL-NOTICE. */

#include "sg_capacity.h"

#include "libltfs/ltfs_error.h"
#include "tape_drivers/tape_drivers.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LOG_PAGE_HEADER_SIZE 4U
#define LOG_PARAMETER_HEADER_SIZE 4U
#define MIN_PARTITION_RECORD_SIZE 8U
#define MAX_PARTITIONS 2U

#define PARAMETER_MAXIMUM_CAPACITY 0x0202U
#define PARAMETER_USED_CAPACITY 0x0203U
#define PARAMETER_REMAINING_CAPACITY 0x0204U

bool sg_capacity_uses_page31(int vendor, int drive_type)
{
	/* HP LTO-6 uses the same MiB-valued Tape Capacity page as LTO-5.
	 * Keep other vendors and newer generations on Volume Statistics. */
	return IS_LTO(drive_type) && (DRIVE_GEN(drive_type) == 5 ||
		(vendor == VENDOR_HP && DRIVE_GEN(drive_type) == 6));
}

struct capacity_parameter {
	const unsigned char *value;
	size_t size;
	bool present;
};

struct partition_capacities {
	uint32_t value[MAX_PARTITIONS];
	size_t count;
};

static uint16_t read_be16(const unsigned char *value)
{
	return ((uint16_t)value[0] << 8) | (uint16_t)value[1];
}

static uint32_t read_be32(const unsigned char *value)
{
	return ((uint32_t)value[0] << 24) |
		((uint32_t)value[1] << 16) |
		((uint32_t)value[2] << 8) |
		(uint32_t)value[3];
}

static int remember_parameter(struct capacity_parameter *parameter,
	const unsigned char *value, size_t size)
{
	if (parameter->present)
		return -EDEV_INTERNAL_ERROR;
	parameter->value = value;
	parameter->size = size;
	parameter->present = true;
	return 0;
}

static int find_capacity_parameters(const unsigned char *page, size_t size,
	struct capacity_parameter *maximum,
	struct capacity_parameter *used,
	struct capacity_parameter *remaining)
{
	size_t offset;
	size_t total_size;

	if (size < LOG_PAGE_HEADER_SIZE || (page[0] & 0x3fU) != 0x17U ||
		page[1] != 0)
		return -EDEV_INTERNAL_ERROR;
	total_size = (size_t)read_be16(page + 2) + LOG_PAGE_HEADER_SIZE;
	if (total_size != size)
		return -EDEV_INTERNAL_ERROR;

	for (offset = LOG_PAGE_HEADER_SIZE; offset < total_size;) {
		uint16_t code;
		size_t parameter_size;
		int result = 0;

		if (total_size - offset < LOG_PARAMETER_HEADER_SIZE)
			return -EDEV_INTERNAL_ERROR;
		code = read_be16(page + offset);
		parameter_size = page[offset + 3];
		offset += LOG_PARAMETER_HEADER_SIZE;
		if (parameter_size > total_size - offset)
			return -EDEV_INTERNAL_ERROR;

		switch (code) {
		case PARAMETER_MAXIMUM_CAPACITY:
			result = remember_parameter(maximum, page + offset,
				parameter_size);
			break;
		case PARAMETER_USED_CAPACITY:
			result = remember_parameter(used, page + offset,
				parameter_size);
			break;
		case PARAMETER_REMAINING_CAPACITY:
			result = remember_parameter(remaining, page + offset,
				parameter_size);
			break;
		default:
			break;
		}
		if (result)
			return result;
		offset += parameter_size;
	}
	return 0;
}

static int parse_partition_records(const struct capacity_parameter *parameter,
	struct partition_capacities *capacities)
{
	size_t offset = 0;

	if (!parameter->present)
		return -EDEV_INTERNAL_ERROR;
	while (offset < parameter->size) {
		const unsigned char *record = parameter->value + offset;
		size_t record_size;

		if (capacities->count == MAX_PARTITIONS ||
			parameter->size - offset < MIN_PARTITION_RECORD_SIZE)
			return -EDEV_INTERNAL_ERROR;
		record_size = (size_t)record[0] + 1;
		if (record_size < MIN_PARTITION_RECORD_SIZE ||
			record_size > parameter->size - offset ||
			read_be16(record + 2) != capacities->count)
			return -EDEV_INTERNAL_ERROR;
		capacities->value[capacities->count] = read_be32(record + 4);
		++capacities->count;
		offset += record_size;
	}
	if (!capacities->count)
		return -EDEV_INTERNAL_ERROR;
	return 0;
}

static uint64_t megabytes_to_mebibytes(uint32_t megabytes)
{
	return ((uint64_t)megabytes * UINT64_C(1000) * UINT64_C(1000)) >> 20;
}

int sg_parse_capacity_page31(const unsigned char *page, size_t size,
	unsigned int capacity_offset, struct tc_remaining_cap *capacity)
{
	/* Parameter codes 1..4: remaining0, remaining1, maximum0, maximum1. */
	struct capacity_parameter parameters[4] = {{0}};
	uint32_t values[4];
	size_t offset, i;

	if (!capacity)
		return -LTFS_NULL_ARG;
	memset(capacity, 0, sizeof(*capacity));
	if (!page)
		return -LTFS_NULL_ARG;
	if (size < LOG_PAGE_HEADER_SIZE || (page[0] & 0x3fU) != 0x31U ||
		page[1] != 0 ||
		(size_t)read_be16(page + 2) + LOG_PAGE_HEADER_SIZE != size)
		return -EDEV_INTERNAL_ERROR;

	for (offset = LOG_PAGE_HEADER_SIZE; offset < size;) {
		uint16_t code;
		size_t parameter_size;

		if (size - offset < LOG_PARAMETER_HEADER_SIZE)
			return -EDEV_INTERNAL_ERROR;
		code = read_be16(page + offset);
		parameter_size = page[offset + 3];
		offset += LOG_PARAMETER_HEADER_SIZE;
		if (parameter_size > size - offset)
			return -EDEV_INTERNAL_ERROR;
		if (code >= 1 && code <= 4) {
			if (parameter_size != sizeof(uint32_t) ||
				remember_parameter(&parameters[code - 1], page + offset,
					parameter_size) < 0)
				return -EDEV_INTERNAL_ERROR;
		}
		offset += parameter_size;
	}
	for (i = 0; i < 4; ++i) {
		if (!parameters[i].present)
			return -EDEV_INTERNAL_ERROR;
		values[i] = read_be32(parameters[i].value);
	}
	if (values[0] > values[2] || values[1] > values[3])
		return -EDEV_INTERNAL_ERROR;

	/* Unlike page17, these values are already MiB: never convert decimal MB. */
	capacity->remaining_p0 = values[0];
	capacity->remaining_p1 = capacity_offset >= values[1] ?
		0 : values[1] - capacity_offset;
	capacity->max_p0 = values[2];
	capacity->max_p1 = values[3];
	return 0;
}

int sg_parse_capacity_page17(const unsigned char *page, size_t size,
	unsigned int capacity_offset, struct tc_remaining_cap *capacity)
{
	struct capacity_parameter maximum = {0};
	struct capacity_parameter used = {0};
	struct capacity_parameter remaining = {0};
	struct partition_capacities maximum_values = {0};
	struct partition_capacities selected_values = {0};
	uint32_t remaining_values[MAX_PARTITIONS] = {0};
	bool selected_is_used;
	size_t i;
	int result;

	if (!page || !capacity)
		return -LTFS_NULL_ARG;
	memset(capacity, 0, sizeof(*capacity));

	result = find_capacity_parameters(page, size, &maximum, &used,
		&remaining);
	if (result)
		return result;
	result = parse_partition_records(&maximum, &maximum_values);
	if (result)
		return result;
	selected_is_used = !remaining.present;
	result = parse_partition_records(selected_is_used ? &used : &remaining,
		&selected_values);
	if (result || maximum_values.count != selected_values.count)
		return -EDEV_INTERNAL_ERROR;

	for (i = 0; i < maximum_values.count; ++i) {
		if (selected_values.value[i] > maximum_values.value[i])
			return -EDEV_INTERNAL_ERROR;
		remaining_values[i] = selected_is_used ?
			maximum_values.value[i] - selected_values.value[i] :
			selected_values.value[i];
	}
	if (maximum_values.count == MAX_PARTITIONS) {
		if (capacity_offset >= remaining_values[1])
			remaining_values[1] = 0;
		else
			remaining_values[1] -= capacity_offset;
	}

	capacity->max_p0 = megabytes_to_mebibytes(maximum_values.value[0]);
	capacity->remaining_p0 = megabytes_to_mebibytes(remaining_values[0]);
	if (maximum_values.count == MAX_PARTITIONS) {
		capacity->max_p1 = megabytes_to_mebibytes(maximum_values.value[1]);
		capacity->remaining_p1 = megabytes_to_mebibytes(remaining_values[1]);
	}
	return 0;
}
