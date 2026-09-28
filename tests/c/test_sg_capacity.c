/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "tape_drivers/linux/sg/sg_capacity.h"
#include "tape_drivers/tape_drivers.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PARAM_MAXIMUM 0x0202
#define PARAM_USED 0x0203
#define PARAM_REMAINING 0x0204

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

static size_t append_capacity_parameter(unsigned char *page, size_t offset,
	uint16_t code, size_t partitions, uint32_t partition0,
	uint32_t partition1)
{
	size_t i;

	put16(page + offset, code);
	page[offset + 2] = 0;
	page[offset + 3] = (unsigned char)(partitions * 8);
	offset += 4;
	for (i = 0; i < partitions; ++i) {
		page[offset] = 7;
		page[offset + 1] = 0;
		put16(page + offset + 2, (uint16_t)i);
		put32(page + offset + 4, i ? partition1 : partition0);
		offset += 8;
	}
	return offset;
}

static size_t append_extended_capacity_parameter(unsigned char *page,
	size_t offset, uint16_t code, uint32_t partition0)
{
	put16(page + offset, code);
	page[offset + 2] = 0;
	page[offset + 3] = 9;
	offset += 4;
	page[offset] = 8;
	page[offset + 1] = 0;
	put16(page + offset + 2, 0);
	put32(page + offset + 4, partition0);
	page[offset + 8] = 0xA5;
	return offset + 9;
}

static size_t finish_page(unsigned char *page, size_t size)
{
	page[0] = 0x17;
	page[1] = 0;
	put16(page + 2, (uint16_t)(size - 4));
	return size;
}

static size_t build_page(unsigned char *page, size_t page_size,
	size_t partitions, uint16_t secondary_code, uint32_t max0,
	uint32_t max1, uint32_t secondary0, uint32_t secondary1)
{
	size_t offset = 4;

	memset(page, 0, page_size);
	offset = append_capacity_parameter(page, offset, PARAM_MAXIMUM,
		partitions, max0, max1);
	if (secondary_code)
		offset = append_capacity_parameter(page, offset, secondary_code,
			partitions, secondary0, secondary1);
	return finish_page(page, offset);
}

static int test_hpe_preformat_used_capacity_single_partition(void)
{
	unsigned char page[64];
	struct tc_remaining_cap capacity;
	size_t size = build_page(page, sizeof(page), 1, PARAM_USED,
		1000, 0, 250, 0);

	CHECK_INT_EQ(sg_parse_capacity_page17(page, size, 0, &capacity), 0);
	CHECK_INT_EQ(capacity.max_p0, 953);
	CHECK_INT_EQ(capacity.remaining_p0, 715);
	CHECK_INT_EQ(capacity.max_p1, 0);
	CHECK_INT_EQ(capacity.remaining_p1, 0);
	return 0;
}

static int test_extended_partition_records_are_accepted(void)
{
	unsigned char page[64] = {0};
	struct tc_remaining_cap capacity;
	size_t offset = 4;

	offset = append_extended_capacity_parameter(page, offset, PARAM_MAXIMUM,
		1000);
	offset = append_extended_capacity_parameter(page, offset, PARAM_USED,
		250);
	finish_page(page, offset);
	CHECK_INT_EQ(sg_parse_capacity_page17(page, offset, 0, &capacity), 0);
	CHECK_INT_EQ(capacity.max_p0, 953);
	CHECK_INT_EQ(capacity.remaining_p0, 715);
	return 0;
}

static int test_remaining_capacity_keeps_priority(void)
{
	unsigned char page[96] = {0};
	struct tc_remaining_cap capacity;
	size_t offset = 4;

	offset = append_capacity_parameter(page, offset, PARAM_MAXIMUM, 2,
		3000, 4000);
	offset = append_capacity_parameter(page, offset, PARAM_USED, 2,
		500, 1000);
	offset = append_capacity_parameter(page, offset, PARAM_REMAINING, 2,
		1000, 2000);
	finish_page(page, offset);

	CHECK_INT_EQ(sg_parse_capacity_page17(page, offset, 0, &capacity), 0);
	CHECK_INT_EQ(capacity.max_p0, 2861);
	CHECK_INT_EQ(capacity.max_p1, 3814);
	CHECK_INT_EQ(capacity.remaining_p0, 953);
	CHECK_INT_EQ(capacity.remaining_p1, 1907);
	return 0;
}

static int test_used_capacity_applies_raw_partition1_offset(void)
{
	unsigned char page[96];
	struct tc_remaining_cap capacity;
	size_t size = build_page(page, sizeof(page), 2, PARAM_USED,
		3000, 4000, 500, 1000);

	CHECK_INT_EQ(sg_parse_capacity_page17(page, size, 100, &capacity), 0);
	CHECK_INT_EQ(capacity.remaining_p0, 2384);
	CHECK_INT_EQ(capacity.remaining_p1, 2765);

	CHECK_INT_EQ(sg_parse_capacity_page17(page, size, 4000, &capacity), 0);
	CHECK_INT_EQ(capacity.remaining_p0, 2384);
	CHECK_INT_EQ(capacity.remaining_p1, 0);
	return 0;
}

static int test_missing_or_invalid_capacity_is_rejected(void)
{
	unsigned char page[96];
	struct tc_remaining_cap capacity;
	size_t size;

	size = build_page(page, sizeof(page), 1, 0, 1000, 0, 0, 0);
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);

	size = build_page(page, sizeof(page), 1, PARAM_USED,
		1000, 0, 1001, 0);
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);

	size = build_page(page, sizeof(page), 1, PARAM_REMAINING,
		1000, 0, 1001, 0);
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);

	CHECK_TRUE(sg_parse_capacity_page17(NULL, size, 0, &capacity) < 0);
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, NULL) < 0);
	CHECK_TRUE(sg_parse_capacity_page17(page, 3, 0, &capacity) < 0);
	return 0;
}

static int test_page_and_parameter_bounds_are_enforced(void)
{
	unsigned char page[96];
	struct tc_remaining_cap capacity;
	size_t size = build_page(page, sizeof(page), 1, PARAM_USED,
		1000, 0, 250, 0);

	put16(page + 2, (uint16_t)(size - 3));
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);
	finish_page(page, size);
	CHECK_TRUE(sg_parse_capacity_page17(page, size - 1, 0, &capacity) < 0);

	finish_page(page, size);
	page[7] = 9;
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);

	size = build_page(page, sizeof(page), 1, PARAM_USED,
		1000, 0, 250, 0);
	page[8] = 6;
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);

	size = build_page(page, sizeof(page), 1, PARAM_USED,
		1000, 0, 250, 0);
	page[7] = 9;
	page[4 + 4 + 8] = 0xA5;
	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);
	return 0;
}

static int test_partition_cardinality_and_order_must_match(void)
{
	unsigned char page[96] = {0};
	struct tc_remaining_cap capacity;
	size_t offset = 4;

	offset = append_capacity_parameter(page, offset, PARAM_MAXIMUM, 1,
		1000, 0);
	offset = append_capacity_parameter(page, offset, PARAM_USED, 2,
		250, 0);
	finish_page(page, offset);
	CHECK_TRUE(sg_parse_capacity_page17(page, offset, 0, &capacity) < 0);

	offset = build_page(page, sizeof(page), 2, PARAM_USED,
		3000, 4000, 500, 1000);
	put16(page + 4 + 4 + 8 + 2, 7);
	CHECK_TRUE(sg_parse_capacity_page17(page, offset, 0, &capacity) < 0);

	offset = build_page(page, sizeof(page), 2, PARAM_USED,
		3000, 4000, 500, 1000);
	put16(page + 4 + 4 + 8 + 2, 0);
	CHECK_TRUE(sg_parse_capacity_page17(page, offset, 0, &capacity) < 0);
	return 0;
}

static int test_malformed_remaining_does_not_fall_back_to_used(void)
{
	unsigned char page[96] = {0};
	struct tc_remaining_cap capacity;
	size_t offset = 4;
	size_t remaining_payload;

	offset = append_capacity_parameter(page, offset, PARAM_MAXIMUM, 1,
		1000, 0);
	offset = append_capacity_parameter(page, offset, PARAM_USED, 1,
		250, 0);
	remaining_payload = offset + 4;
	offset = append_capacity_parameter(page, offset, PARAM_REMAINING, 1,
		500, 0);
	page[remaining_payload] = 6;
	finish_page(page, offset);
	CHECK_TRUE(sg_parse_capacity_page17(page, offset, 0, &capacity) < 0);
	return 0;
}

static int test_more_than_two_records_is_rejected(void)
{
	unsigned char page[96];
	struct tc_remaining_cap capacity;
	size_t size = build_page(page, sizeof(page), 3, PARAM_USED,
		1000, 500, 250, 100);

	CHECK_TRUE(sg_parse_capacity_page17(page, size, 0, &capacity) < 0);
	return 0;
}

static int test_capacity_page_selection(void)
{
	const int vendors[] = {VENDOR_UNKNOWN, VENDOR_IBM, VENDOR_HP, VENDOR_QUANTUM};
	const int modern[] = {DRIVE_LTO7, DRIVE_LTO7_HH, DRIVE_LTO8,
		DRIVE_LTO8_HH, DRIVE_LTO9, DRIVE_LTO9_HH};
	size_t i, j;

	for (i = 0; i < sizeof(vendors) / sizeof(vendors[0]); ++i) {
		CHECK_TRUE(sg_capacity_uses_page31(vendors[i], DRIVE_LTO5));
		CHECK_TRUE(sg_capacity_uses_page31(vendors[i], DRIVE_LTO5_HH));
		CHECK_INT_EQ(sg_capacity_uses_page31(vendors[i], DRIVE_LTO6),
			vendors[i] == VENDOR_HP);
		CHECK_INT_EQ(sg_capacity_uses_page31(vendors[i], DRIVE_LTO6_HH),
			vendors[i] == VENDOR_HP);
		CHECK_TRUE(!sg_capacity_uses_page31(vendors[i], 0));
		for (j = 0; j < sizeof(modern) / sizeof(modern[0]); ++j)
			CHECK_TRUE(!sg_capacity_uses_page31(vendors[i], modern[j]));
	}
	return 0;
}

static size_t append_page31_parameter(unsigned char *page, size_t offset,
	uint16_t code, uint32_t value, unsigned char width)
{
	put16(page + offset, code);
	page[offset + 2] = 0;
	page[offset + 3] = width;
	put32(page + offset + 4, value);
	return offset + 4 + width;
}

static size_t build_page31(unsigned char *page, size_t page_size)
{
	size_t offset = 4;

	memset(page, 0, page_size);
	/* Page31 values are MiB, unlike the decimal MB values of page17. */
	offset = append_page31_parameter(page, offset, 1, 1000, 4);
	offset = append_page31_parameter(page, offset, 2, 2000, 4);
	offset = append_page31_parameter(page, offset, 3, 3000, 4);
	offset = append_page31_parameter(page, offset, 4, 4000, 4);
	finish_page(page, offset);
	page[0] = 0x31;
	return offset;
}

static int page31_rejection_is_zeroed(const unsigned char *page, size_t size,
	unsigned int offset)
{
	struct tc_remaining_cap capacity;

	memset(&capacity, 0xa5, sizeof(capacity));
	CHECK_TRUE(sg_parse_capacity_page31(page, size, offset, &capacity) < 0);
	CHECK_INT_EQ(capacity.max_p0, 0);
	CHECK_INT_EQ(capacity.max_p1, 0);
	CHECK_INT_EQ(capacity.remaining_p0, 0);
	CHECK_INT_EQ(capacity.remaining_p1, 0);
	return 0;
}

static int test_page31_preserves_mib_and_clamps_only_partition1(void)
{
	unsigned char page[96];
	struct tc_remaining_cap capacity;
	size_t size = build_page31(page, sizeof(page));
	const unsigned int offsets[] = {0, 100, 2000, UINT32_MAX};
	const unsigned int expected[] = {2000, 1900, 0, 0};
	size_t i;

	for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
		CHECK_INT_EQ(sg_parse_capacity_page31(page, size, offsets[i], &capacity), 0);
		CHECK_INT_EQ(capacity.max_p0, 3000);
		CHECK_INT_EQ(capacity.max_p1, 4000);
		CHECK_INT_EQ(capacity.remaining_p0, 1000);
		CHECK_INT_EQ(capacity.remaining_p1, expected[i]);
	}
	put32(page + 8, UINT32_MAX);
	put32(page + 24, UINT32_MAX);
	CHECK_INT_EQ(sg_parse_capacity_page31(page, size, 0, &capacity), 0);
	CHECK_TRUE(capacity.max_p0 == UINT32_MAX);
	CHECK_TRUE(capacity.remaining_p0 == UINT32_MAX);
	return 0;
}

static int test_page31_missing_duplicate_and_wrong_width_are_rejected(void)
{
	unsigned char page[96];
	const unsigned char widths[] = {0, 1, 2, 3, 5};
	size_t size, i;
	uint16_t code, changed;

	for (changed = 1; changed <= 4; ++changed) {
		size = build_page31(page, sizeof(page));
		put16(page + 4 + (changed - 1) * 8, 0x1234);
		CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, 0), 0);

		size = build_page31(page, sizeof(page));
		size = append_page31_parameter(page, size, changed, 0, 4);
		put16(page + 2, (uint16_t)(size - 4));
		CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, 0), 0);

		for (i = 0; i < sizeof(widths) / sizeof(widths[0]); ++i) {
			memset(page, 0, sizeof(page));
			size = 4;
			for (code = 1; code <= 4; ++code)
				size = append_page31_parameter(page, size, code,
					code * 1000, code == changed ? widths[i] : 4);
			finish_page(page, size);
			page[0] = 0x31;
			CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, 0), 0);
		}
	}
	return 0;
}

static int test_page31_exact_framing_and_null_inputs_are_enforced(void)
{
	unsigned char page[96];
	size_t size = build_page31(page, sizeof(page));
	size_t i;

	for (i = 0; i < size; ++i)
		CHECK_INT_EQ(page31_rejection_is_zeroed(page, i, 0), 0);
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size + 1, 0), 0);
	CHECK_INT_EQ(page31_rejection_is_zeroed(NULL, size, 0), 0);
	CHECK_TRUE(sg_parse_capacity_page31(page, size, 0, NULL) < 0);
	page[0] = 0x17;
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, 0), 0);
	page[0] = 0x31;
	page[1] = 1;
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, 0), 0);
	page[1] = 0;
	put16(page + 2, (uint16_t)(size - 3));
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size + 1, 0), 0);
	put16(page + 2, (uint16_t)size);
	put16(page + size, 0x1234);
	page[size + 3] = 5;
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size + 4, 0), 0);
	return 0;
}

static int test_page31_inconsistent_remaining_is_rejected_before_offset(void)
{
	unsigned char page[96];
	size_t size = build_page31(page, sizeof(page));

	put32(page + 8, 3001);
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, 0), 0);
	build_page31(page, sizeof(page));
	put32(page + 16, 4001);
	CHECK_INT_EQ(page31_rejection_is_zeroed(page, size, UINT32_MAX), 0);
	return 0;
}

static int test_page31_unordered_parameters_and_unknown_fields_are_allowed(void)
{
	unsigned char page[96] = {0};
	struct tc_remaining_cap capacity;
	size_t size = 4;

	size = append_page31_parameter(page, size, 4, 4000, 4);
	size = append_page31_parameter(page, size, 2, 0, 4);
	size = append_page31_parameter(page, size, 0x1234, 0, 0);
	size = append_page31_parameter(page, size, 3, 3000, 4);
	size = append_page31_parameter(page, size, 1, 0, 4);
	finish_page(page, size);
	page[0] = 0x31;
	CHECK_INT_EQ(sg_parse_capacity_page31(page, size, 0, &capacity), 0);
	CHECK_INT_EQ(capacity.max_p0, 3000);
	CHECK_INT_EQ(capacity.max_p1, 4000);
	CHECK_INT_EQ(capacity.remaining_p0, 0);
	CHECK_INT_EQ(capacity.remaining_p1, 0);
	return 0;
}

int main(void)
{
	CHECK_INT_EQ(test_page31_preserves_mib_and_clamps_only_partition1(), 0);
	CHECK_INT_EQ(test_page31_missing_duplicate_and_wrong_width_are_rejected(), 0);
	CHECK_INT_EQ(test_page31_exact_framing_and_null_inputs_are_enforced(), 0);
	CHECK_INT_EQ(test_page31_inconsistent_remaining_is_rejected_before_offset(), 0);
	CHECK_INT_EQ(test_page31_unordered_parameters_and_unknown_fields_are_allowed(), 0);
	CHECK_INT_EQ(test_capacity_page_selection(), 0);
	CHECK_INT_EQ(test_hpe_preformat_used_capacity_single_partition(), 0);
	CHECK_INT_EQ(test_extended_partition_records_are_accepted(), 0);
	CHECK_INT_EQ(test_remaining_capacity_keeps_priority(), 0);
	CHECK_INT_EQ(test_used_capacity_applies_raw_partition1_offset(), 0);
	CHECK_INT_EQ(test_missing_or_invalid_capacity_is_rejected(), 0);
	CHECK_INT_EQ(test_page_and_parameter_bounds_are_enforced(), 0);
	CHECK_INT_EQ(test_partition_cardinality_and_order_must_match(), 0);
	CHECK_INT_EQ(test_malformed_remaining_does_not_fall_back_to_used(), 0);
	CHECK_INT_EQ(test_more_than_two_records_is_rejected(), 0);
	return 0;
}
