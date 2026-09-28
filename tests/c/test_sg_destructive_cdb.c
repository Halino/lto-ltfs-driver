/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "tape_drivers/linux/sg/sg_scsi_tape.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int check_cdb(const uint8_t actual[CDB6_LEN],
	const uint8_t expected[CDB6_LEN])
{
	CHECK_TRUE(memcmp(actual, expected, CDB6_LEN) == 0);
	return 0;
}

static int test_erase_cdbs_are_literal_and_closed(void)
{
	uint8_t actual[CDB6_LEN];
	static const uint8_t short_expected[CDB6_LEN] = {
		ERASE, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const uint8_t long_expected[CDB6_LEN] = {
		ERASE, 0x03, 0x00, 0x00, 0x00, 0x00
	};

	memset(actual, 0xA5, sizeof(actual));
	CHECK_INT_EQ(sg_build_erase_cdb(false, actual), 0);
	CHECK_INT_EQ(check_cdb(actual, short_expected), 0);

	memset(actual, 0xA5, sizeof(actual));
	CHECK_INT_EQ(sg_build_erase_cdb(true, actual), 0);
	CHECK_INT_EQ(check_cdb(actual, long_expected), 0);
	CHECK_INT_EQ(sg_build_erase_cdb(false, NULL), -LTFS_NULL_ARG);
	return 0;
}

static int test_format_cdbs_are_literal_and_invalid_type_is_rejected(void)
{
	uint8_t actual[CDB6_LEN];
	TC_FORMAT_TYPE type;

	for (type = TC_FORMAT_DEFAULT; type < TC_FORMAT_MAX; ++type) {
		uint8_t expected[CDB6_LEN] = {
			FORMAT_MEDIUM, 0x00, (uint8_t)type, 0x00, 0x00, 0x00
		};
		memset(actual, 0xA5, sizeof(actual));
		CHECK_INT_EQ(sg_build_format_cdb(type, actual), 0);
		CHECK_INT_EQ(check_cdb(actual, expected), 0);
	}

	memset(actual, 0xA5, sizeof(actual));
	CHECK_INT_EQ(sg_build_format_cdb(TC_FORMAT_MAX, actual), -EDEV_INVALID_ARG);
	CHECK_TRUE(actual[0] == 0xA5);
	CHECK_INT_EQ(
		sg_build_format_cdb(TC_FORMAT_DEFAULT, NULL), -LTFS_NULL_ARG);
	return 0;
}

int main(void)
{
	CHECK_INT_EQ(test_erase_cdbs_are_literal_and_closed(), 0);
	CHECK_INT_EQ(test_format_cdbs_are_literal_and_invalid_type_is_rejected(), 0);
	return 0;
}
