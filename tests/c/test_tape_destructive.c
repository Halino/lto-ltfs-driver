/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "tape.h"
#include "tape_drivers/tape_drivers.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct destructive_backend {
	int load_result;
	int modesense_result;
	int modeselect_result;
	int format_result;
	int erase_result;
	int locate_result;
	int modeselect_failure;
	unsigned int modeselect_fail_call;
	unsigned int load_calls;
	unsigned int modesense_calls;
	unsigned int modeselect_calls;
	unsigned int format_calls;
	unsigned int erase_calls;
	unsigned int locate_calls;
	bool last_long_erase;
	TC_FORMAT_TYPE last_format;
};

static int fake_load(void *context, struct tc_position *position)
{
	struct destructive_backend *backend = context;
	++backend->load_calls;
	memset(position, 0, sizeof(*position));
	return backend->load_result;
}

static int fake_modesense(void *context, const uint8_t page,
	const TC_MP_PC_TYPE pc, const uint8_t subpage, unsigned char *buffer,
	const size_t size)
{
	struct destructive_backend *backend = context;
	(void)page;
	(void)pc;
	(void)subpage;
	++backend->modesense_calls;
	memset(buffer, 0, size);
	if (size > 2)
		buffer[2] = TC_MP_JB;
	return backend->modesense_result < 0 ? backend->modesense_result : (int)size;
}

static int fake_modeselect(void *context, unsigned char *buffer,
	const size_t size)
{
	struct destructive_backend *backend = context;
	(void)buffer;
	(void)size;
	++backend->modeselect_calls;
	if (backend->modeselect_calls == backend->modeselect_fail_call)
		return backend->modeselect_failure;
	return backend->modeselect_result;
}

static int fake_format(void *context, TC_FORMAT_TYPE format,
	const char *volume_name, const char *barcode_name,
	const char *volume_mam_uuid)
{
	struct destructive_backend *backend = context;
	CHECK_TRUE(volume_name == NULL);
	CHECK_TRUE(barcode_name == NULL);
	CHECK_TRUE(volume_mam_uuid == NULL);
	++backend->format_calls;
	backend->last_format = format;
	return backend->format_result;
}

static int fake_erase(void *context, struct tc_position *position,
	bool long_erase)
{
	struct destructive_backend *backend = context;
	++backend->erase_calls;
	backend->last_long_erase = long_erase;
	position->block = 91;
	return backend->erase_result;
}

static int fake_locate(void *context, struct tc_position destination,
	struct tc_position *position)
{
	struct destructive_backend *backend = context;
	++backend->locate_calls;
	*position = destination;
	return backend->locate_result;
}

static void prepare(struct device_data *device, struct tape_ops *ops,
	struct destructive_backend *backend)
{
	memset(device, 0, sizeof(*device));
	memset(ops, 0, sizeof(*ops));
	memset(backend, 0, sizeof(*backend));
	ops->load = fake_load;
	ops->modesense = fake_modesense;
	ops->modeselect = fake_modeselect;
	ops->format = fake_format;
	ops->erase = fake_erase;
	ops->locate = fake_locate;
	device->backend = ops;
	device->backend_data = backend;
	backend->last_format = TC_FORMAT_MAX;
}

static int test_short_and_long_erase_propagate_failure_without_replay(void)
{
	struct destructive_backend backend;
	struct device_data device;
	struct tape_ops ops;

	prepare(&device, &ops, &backend);
	CHECK_INT_EQ(tape_erase(&device, false), 0);
	CHECK_INT_EQ(backend.erase_calls, 1);
	CHECK_TRUE(!backend.last_long_erase);
	CHECK_INT_EQ(device.position.block, 91);

	backend.erase_result = -EIO;
	CHECK_INT_EQ(tape_erase(&device, true), -EIO);
	CHECK_INT_EQ(backend.erase_calls, 2);
	CHECK_TRUE(backend.last_long_erase);
	return 0;
}

static int test_partition_and_destructive_format_types_are_exact(void)
{
	struct destructive_backend backend;
	struct device_data device;
	struct tape_ops ops;

	prepare(&device, &ops, &backend);
	CHECK_INT_EQ(tape_format(&device, 1, 0, false), 0);
	CHECK_INT_EQ(backend.load_calls, 1);
	CHECK_INT_EQ(backend.modesense_calls, 1);
	CHECK_INT_EQ(backend.modeselect_calls, 1);
	CHECK_INT_EQ(backend.format_calls, 1);
	CHECK_INT_EQ(backend.last_format, TC_FORMAT_PARTITION);

	prepare(&device, &ops, &backend);
	CHECK_INT_EQ(tape_format(&device, 0, 0, true), 0);
	CHECK_INT_EQ(backend.last_format, TC_FORMAT_DEST_PART);
	return 0;
}

static int test_format_refuses_after_each_failed_boundary(void)
{
	struct destructive_backend backend;
	struct device_data device;
	struct tape_ops ops;

	prepare(&device, &ops, &backend);
	backend.load_result = -EIO;
	CHECK_INT_EQ(tape_format(&device, 1, 0, false), -EIO);
	CHECK_INT_EQ(backend.modesense_calls, 0);
	CHECK_INT_EQ(backend.format_calls, 0);

	prepare(&device, &ops, &backend);
	backend.modesense_result = -EIO;
	CHECK_INT_EQ(tape_format(&device, 1, 0, false), -EIO);
	CHECK_INT_EQ(backend.modeselect_calls, 0);
	CHECK_INT_EQ(backend.format_calls, 0);

	prepare(&device, &ops, &backend);
	backend.modeselect_result = -EIO;
	CHECK_INT_EQ(tape_format(&device, 1, 0, false), -EIO);
	CHECK_INT_EQ(backend.format_calls, 0);

	prepare(&device, &ops, &backend);
	backend.modeselect_fail_call = 1;
	backend.modeselect_failure = -EIO;
	CHECK_INT_EQ(tape_format(&device, 1, 0x5E, false), -EIO);
	CHECK_INT_EQ(backend.modeselect_calls, 1);
	CHECK_INT_EQ(backend.format_calls, 0);

	prepare(&device, &ops, &backend);
	backend.format_result = -EIO;
	CHECK_INT_EQ(tape_format(&device, 1, 0, false), -EIO);
	CHECK_INT_EQ(backend.format_calls, 1);
	return 0;
}

static int test_unformat_uses_distinct_commands_and_refuses_after_failed_locate(void)
{
	struct destructive_backend backend;
	struct device_data device;
	struct tape_ops ops;

	prepare(&device, &ops, &backend);
	CHECK_INT_EQ(tape_unformat(&device), 0);
	CHECK_INT_EQ(backend.load_calls, 1);
	CHECK_INT_EQ(backend.last_format, TC_FORMAT_PARTITION);

	prepare(&device, &ops, &backend);
	CHECK_INT_EQ(tape_unformat_hard(&device), 0);
	CHECK_INT_EQ(backend.locate_calls, 1);
	CHECK_INT_EQ(backend.load_calls, 0);
	CHECK_INT_EQ(backend.last_format, TC_FORMAT_DEFAULT);

	prepare(&device, &ops, &backend);
	backend.locate_result = -EIO;
	CHECK_INT_EQ(tape_unformat_hard(&device), -EIO);
	CHECK_INT_EQ(backend.format_calls, 0);
	return 0;
}

int main(void)
{
	CHECK_INT_EQ(
		test_short_and_long_erase_propagate_failure_without_replay(), 0);
	CHECK_INT_EQ(test_partition_and_destructive_format_types_are_exact(), 0);
	CHECK_INT_EQ(test_format_refuses_after_each_failed_boundary(), 0);
	CHECK_INT_EQ(
		test_unformat_uses_distinct_commands_and_refuses_after_failed_locate(),
		0);
	return 0;
}
