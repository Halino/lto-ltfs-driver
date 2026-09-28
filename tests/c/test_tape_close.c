/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "ltfs.h"
#include "ltfs_internal.h"
#include "tape.h"
#include "periodic_sync.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

static int close_calls;
static int unload_calls;
static int load_calls;
static int modeselect_calls;
static int close_result = -EIO;
static bool aom_active;

struct coherency_fixture {
	unsigned char data[75];
	tape_partition_t partition;
	int write_calls;
	int read_calls;
	int write_result;
	int building_index;
	int unmounting;
};

static int capture_coherency(void *backend_data, const tape_partition_t partition,
	const unsigned char *buffer, const size_t size)
{
	struct coherency_fixture *fixture = backend_data;

	CHECK_INT_EQ(size, sizeof(fixture->data));
	memcpy(fixture->data, buffer, size);
	fixture->partition = partition;
	++fixture->write_calls;
	return fixture->write_result;
}

static int read_captured_coherency(void *backend_data,
	const tape_partition_t partition, const uint16_t attribute,
	unsigned char *buffer, const size_t size)
{
	struct coherency_fixture *fixture = backend_data;

	CHECK_INT_EQ(partition, fixture->partition);
	CHECK_INT_EQ(attribute, 0x080c);
	CHECK_INT_EQ(size, sizeof(fixture->data));
	memcpy(buffer, fixture->data, size);
	++fixture->read_calls;
	return 0;
}

static int test_coherency_serializes_required_nul_and_roundtrips(void)
{
	/* SNIA VCI: ACSI starts at wire offset 32; its offset 4 is a required NUL. */
	static const unsigned char expected_prefix[] = {
		0x08, 0x0c, 0x00, 0x00, 0x46, 0x08,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
		0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
		0x00, 0x2b, 'L', 'T', 'F', 'S', 0x00,
	};
	static const char expected_uuid[] = "12345678-1234-4234-8234-123456789abc";
	struct coherency_fixture fixture = {0};
	struct tape_ops ops = {
		.write_attribute = capture_coherency,
		.read_attribute = read_captured_coherency,
	};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	struct tc_coherency written = {
		.volume_change_ref = UINT64_C(0x0102030405060708),
		.count = UINT64_C(0x1112131415161718),
		.set_id = UINT64_C(0x2122232425262728),
		.uuid = "12345678-1234-4234-8234-123456789abc",
		.version = 1,
	};
	struct tc_coherency read_back = {0};

	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(tape_set_cart_coherency(&device, 1, &written), 0);
	CHECK_INT_EQ(fixture.write_calls, 1);
	CHECK_INT_EQ(fixture.partition, 1);
	CHECK_INT_EQ(fixture.data[36], 0);
	CHECK_INT_EQ(memcmp(fixture.data, expected_prefix, sizeof(expected_prefix)), 0);
	CHECK_INT_EQ(memcmp(fixture.data + 37, expected_uuid, sizeof(expected_uuid)), 0);
	CHECK_INT_EQ(fixture.data[74], 1);
	CHECK_INT_EQ(tape_get_cart_coherency(&device, 1, &read_back), 0);
	CHECK_INT_EQ(fixture.read_calls, 1);
	CHECK_TRUE(read_back.volume_change_ref == UINT64_C(0x0102030405060708));
	CHECK_TRUE(read_back.count == UINT64_C(0x1112131415161718));
	CHECK_TRUE(read_back.set_id == UINT64_C(0x2122232425262728));
	CHECK_INT_EQ(strcmp(read_back.uuid, expected_uuid), 0);
	CHECK_INT_EQ(read_back.version, 1);

	fixture.write_result = -EIO;
	CHECK_INT_EQ(tape_set_cart_coherency(&device, 0, &written), -EIO);
	CHECK_INT_EQ(fixture.write_calls, 2);
	CHECK_INT_EQ(fixture.partition, 0);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int failing_close(void *backend_data)
{
	CHECK_TRUE(backend_data != NULL);
	++close_calls;
	return close_result;
}

static int fake_test_unit_ready(void *backend_data)
{
	CHECK_TRUE(backend_data != NULL);
	return 0;
}

static int fake_modesense(void *backend_data, const uint8_t page,
	const TC_MP_PC_TYPE pc, const uint8_t subpage, unsigned char *buffer,
	const size_t size)
{
	(void)page;
	(void)pc;
	(void)subpage;
	CHECK_TRUE(backend_data != NULL);
	CHECK_TRUE(size > 21);
	memset(buffer, 0, size);
	buffer[21] = aom_active ? 0x10 : 0;
	return (int)size;
}

static int fake_modeselect(void *backend_data, unsigned char *buffer,
	const size_t size)
{
	(void)buffer;
	(void)size;
	CHECK_TRUE(backend_data != NULL);
	++modeselect_calls;
	return 0;
}

static int fake_load(void *backend_data, struct tc_position *position)
{
	(void)position;
	CHECK_TRUE(backend_data != NULL);
	++load_calls;
	return 0;
}

static int fake_unload(void *backend_data, struct tc_position *position)
{
	(void)position;
	CHECK_TRUE(backend_data != NULL);
	++unload_calls;
	return 0;
}

static void prepare_volume(struct ltfs_volume *volume,
	struct device_data *device, struct tape_ops *ops, int *marker)
{
	memset(volume, 0, sizeof(*volume));
	memset(device, 0, sizeof(*device));
	memset(ops, 0, sizeof(*ops));
	ops->close = failing_close;
	ops->test_unit_ready = fake_test_unit_ready;
	ops->modesense = fake_modesense;
	ops->modeselect = fake_modeselect;
	ops->load = fake_load;
	ops->unload = fake_unload;
	device->backend = ops;
	device->backend_data = marker;
	volume->device = device;
}

static void reset_fixture(void)
{
	close_calls = 0;
	unload_calls = 0;
	load_calls = 0;
	modeselect_calls = 0;
	close_result = -EIO;
	aom_active = false;
}

static int test_public_close_propagates_backend_result(void)
{
	struct tape_ops ops;
	struct device_data device;
	struct ltfs_volume volume;
	int marker = 1;

	reset_fixture();
	prepare_volume(&volume, &device, &ops, &marker);
	CHECK_INT_EQ(ltfs_device_close(&volume), -EIO);
	CHECK_INT_EQ(close_calls, 1);
	CHECK_INT_EQ(unload_calls, 0);
	CHECK_TRUE(device.backend == NULL);
	CHECK_TRUE(device.backend_data == NULL);

	prepare_volume(&volume, &device, &ops, &marker);
	close_result = 7;
	CHECK_INT_EQ(ltfs_device_close(&volume), 7);
	CHECK_INT_EQ(close_calls, 2);
	return 0;
}

static int test_unload_is_refused_without_acknowledged_close(void)
{
	struct tape_ops ops;
	struct device_data device;
	struct ltfs_volume volume;
	int marker = 1;

	reset_fixture();
	prepare_volume(&volume, &device, &ops, &marker);
	close_result = 0;
	aom_active = true;
	CHECK_INT_EQ(ltfs_device_close_after_finalization(&volume, false), 0);
	CHECK_INT_EQ(unload_calls, 0);
	CHECK_INT_EQ(load_calls, 0);
	return 0;
}

static int test_acknowledged_close_exercises_aom_path(void)
{
	struct tape_ops ops;
	struct device_data device;
	struct ltfs_volume volume;
	int marker = 1;

	reset_fixture();
	prepare_volume(&volume, &device, &ops, &marker);
	close_result = 0;
	aom_active = true;
	device.append_only_mode = true;
	CHECK_INT_EQ(ltfs_device_close_after_finalization(&volume, true), 0);
	CHECK_INT_EQ(unload_calls, 1);
	CHECK_INT_EQ(load_calls, 1);
	CHECK_INT_EQ(modeselect_calls, 1);
	return 0;
}

static int test_acknowledged_close_skips_inactive_aom_path(void)
{
	struct tape_ops ops;
	struct device_data device;
	struct ltfs_volume volume;
	int marker = 1;

	reset_fixture();
	prepare_volume(&volume, &device, &ops, &marker);
	close_result = 0;
	aom_active = false;
	device.append_only_mode = false;
	CHECK_INT_EQ(ltfs_device_close_after_finalization(&volume, true), 0);
	CHECK_INT_EQ(unload_calls, 0);
	CHECK_INT_EQ(load_calls, 0);
	CHECK_INT_EQ(modeselect_calls, 0);
	return 0;
}

static int test_missing_backend_close_is_observable(void)
{
	struct tape_ops ops;
	struct device_data device;
	struct ltfs_volume volume;
	int marker = 1;

	reset_fixture();
	prepare_volume(&volume, &device, &ops, &marker);
	ops.close = NULL;
	CHECK_INT_EQ(ltfs_device_close_after_finalization(&volume, false),
		-ENOSYS);
	CHECK_TRUE(device.backend == NULL);
	CHECK_TRUE(device.backend_data == NULL);
	return 0;
}

static bool writable_backend(void *context)
{
	(void)context;
	return false;
}

static int full_capacity(void *context, struct tc_remaining_cap *capacity)
{
	(void)context;
	capacity->max_p0 = capacity->max_p1 = 1000;
	capacity->remaining_p0 = capacity->remaining_p1 = 900;
	return 0;
}

static int writable_parameters(void *context, struct tc_drive_param *parameters)
{
	(void)context;
	memset(parameters, 0, sizeof(*parameters));
	parameters->max_blksize = 1024 * 1024;
	return 0;
}

static int test_forced_readonly_blocks_payload_and_filemarks(void)
{
	struct coherency_fixture fixture = {0};
	struct tape_ops ops = {.is_readonly = writable_backend};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(tape_read_only(&device, 0), 0);
	CHECK_TRUE(!tape_is_forced_read_only(&device));
	CHECK_INT_EQ(tape_force_read_only(&device), 0);
	CHECK_TRUE(tape_is_forced_read_only(&device));
	CHECK_INT_EQ(tape_read_only(&device, 0), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(tape_read_only(&device, 1), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(tape_write(&device, "x", 1, true, true), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(tape_write_filemark(&device, 1, true, true, false), -LTFS_WRITE_PROTECT);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int test_forced_readonly_blocks_both_mam_write_entries(void)
{
	struct coherency_fixture fixture = {0};
	struct tape_ops ops = {.write_attribute = capture_coherency};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	struct tc_coherency coherency = {.uuid = "12345678-1234-4234-8234-123456789abc"};
	struct tape_attr attributes = {0};
	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(tape_force_read_only(&device), 0);
	CHECK_INT_EQ(tape_set_cart_coherency(&device, 0, &coherency), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(tape_set_cart_coherency(&device, 1, &coherency), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(tape_set_attribute_to_cm(&device, &attributes, TC_MAM_USER_MEDIUM_LABEL), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(fixture.write_calls, 0);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int test_forced_readonly_survives_load_refresh(void)
{
	int marker = 1;
	struct tape_ops ops = {
		.load = fake_load, .readpos = fake_load,
		.test_unit_ready = fake_test_unit_ready, .set_default = fake_test_unit_ready,
		.remaining_capacity = full_capacity, .get_parameters = writable_parameters,
		.modesense = fake_modesense, .is_readonly = writable_backend,
	};
	struct device_data device = {.backend = &ops, .backend_data = &marker,
		.medium_locked = true};
	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(ltfs_mutex_init(&device.append_pos_mutex), 0);
	CHECK_INT_EQ(tape_load_tape(&device, NULL, true), 0);
	CHECK_INT_EQ(tape_read_only(&device, 0), 0);
	CHECK_INT_EQ(tape_read_only(&device, 1), 0);
	CHECK_INT_EQ(tape_force_read_only(&device), 0);
	for (int refresh = 0; refresh < 3; ++refresh) {
		CHECK_INT_EQ(tape_load_tape(&device, NULL, true), 0);
		CHECK_INT_EQ(tape_read_only(&device, 0), -LTFS_WRITE_PROTECT);
		CHECK_INT_EQ(tape_read_only(&device, 1), -LTFS_WRITE_PROTECT);
	}
	ltfs_mutex_destroy(&device.append_pos_mutex);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int test_writable_recovery_can_update_mam_after_write_error(void)
{
	struct coherency_fixture fixture = {0};
	struct tape_ops ops = {.write_attribute = capture_coherency};
	struct device_data device = {.backend = &ops, .backend_data = &fixture,
		.write_error = true};
	struct tc_coherency coherency = {.uuid = "12345678-1234-4234-8234-123456789abc"};
	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(tape_read_only(&device, 0), -LTFS_WRITE_ERROR);
	CHECK_INT_EQ(tape_set_cart_coherency(&device, 0, &coherency), 0);
	CHECK_INT_EQ(tape_set_cart_coherency(&device, 1, &coherency), 0);
	CHECK_INT_EQ(fixture.write_calls, 2);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int reject_attribute_read(void *context, const tape_partition_t partition,
	const uint16_t attribute, unsigned char *buffer, const size_t size)
{
	struct coherency_fixture *fixture = context;
	(void)partition;
	(void)attribute;
	(void)buffer;
	(void)size;
	++fixture->read_calls;
	return -EIO;
}

static int test_forced_readonly_skips_high_level_coherency(void)
{
	struct coherency_fixture fixture = {0};
	struct tape_ops ops = {.read_attribute = reject_attribute_read};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	struct ltfs_volume volume = {.device = &device};
	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(tape_force_read_only(&device), 0);
	CHECK_INT_EQ(ltfs_update_cart_coherency(&volume), 0);
	CHECK_INT_EQ(fixture.read_calls, 0);
	CHECK_INT_EQ(fixture.write_calls, 0);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int test_forced_readonly_rejects_index_repair_before_device_io(void)
{
	struct coherency_fixture fixture = {0};
	struct tape_ops ops = {.read_attribute = reject_attribute_read};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	struct ltfs_volume volume = {.device = &device};
	CHECK_INT_EQ(ltfs_mutex_init(&device.read_only_flag_mutex), 0);
	CHECK_INT_EQ(tape_force_read_only(&device), 0);
	CHECK_INT_EQ(ltfs_write_index('a', SYNC_WRITE_PERM, &volume), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(ltfs_write_index('a', SYNC_RECOVERY, &volume), -LTFS_WRITE_PROTECT);
	CHECK_INT_EQ(fixture.read_calls, 0);
	CHECK_INT_EQ(fixture.write_calls, 0);
	ltfs_mutex_destroy(&device.read_only_flag_mutex);
	return 0;
}

static int unsupported_lock_attribute(void *context, const tape_partition_t partition,
	const uint16_t attribute, unsigned char *buffer, const size_t size)
{
	struct coherency_fixture *fixture = context;
	(void)partition;
	(void)buffer;
	(void)size;
	++fixture->read_calls;
	CHECK_INT_EQ(attribute, TC_MAM_LOCKED_MAM);
	return -EDEV_INVALID_FIELD_CDB;
}

static int healthy_cartridge(void *context, struct tc_cartridge_health *health)
{
	(void)context;
	memset(health, 0, sizeof(*health));
	return 0;
}

static void observe_readonly_unmount(void *context,
	enum ltfs_finalization_phase phase, const char *message_code)
{
	struct coherency_fixture *fixture = context;
	(void)message_code;
	if (phase == LTFS_FINALIZATION_BUILDING_INDEX)
		++fixture->building_index;
	if (phase == LTFS_FINALIZATION_UNMOUNTING)
		++fixture->unmounting;
}

static int test_forced_readonly_unmount_preserves_index(void)
{
	int failures = 0;
	/* Clean IP, clean DP, and atime-dirty IP must all close without a commit. */
	for (int scenario = 0; scenario < 3; ++scenario) {
		struct coherency_fixture fixture = {0};
		struct tape_ops ops = {
			.read_attribute = unsupported_lock_attribute,
			.write_attribute = capture_coherency,
			.test_unit_ready = fake_test_unit_ready,
			.get_cartridge_health = healthy_cartridge,
		};
		struct ltfs_volume *volume = NULL;
		CHECK_INT_EQ(ltfs_volume_alloc(NULL, &volume), 0);
		volume->device->backend = &ops;
		volume->device->backend_data = &fixture;
		volume->label->partid_ip = 'a';
		volume->label->partid_dp = 'b';
		volume->index->selfptr.partition = scenario == 1 ? 'b' : 'a';
		volume->index->generation = 7;
		volume->index->dirty = false;
		volume->index->use_atime = true;
		volume->index->atime_dirty = scenario == 2;
		volume->mount_type = MOUNT_NORMAL;
		CHECK_INT_EQ(tape_force_read_only(volume->device), 0);
		int result = ltfs_unmount_observed(SYNC_UNMOUNT, volume,
			observe_readonly_unmount, &fixture);
		if (result != 0 || fixture.building_index != 0 || fixture.unmounting != 1 ||
			fixture.write_calls != 0 || volume->index->generation != 7) {
			fprintf(stderr, "readonly unmount scenario %d: result=%d build=%d unmount=%d writes=%d generation=%u\n",
				scenario, result, fixture.building_index, fixture.unmounting,
				fixture.write_calls, volume->index->generation);
			++failures;
		}
		volume->device->backend = NULL;
		volume->device->backend_data = NULL;
		ltfs_volume_free(&volume);
	}
	CHECK_INT_EQ(failures, 0);
	return 0;
}

static int test_periodic_sync_is_not_started_for_forced_readonly(void)
{
	int failures = 0;
	for (int read_only = 0; read_only < 2; ++read_only) {
		struct ltfs_volume *volume = NULL;
		CHECK_INT_EQ(ltfs_volume_alloc(NULL, &volume), 0);
		if (read_only)
			CHECK_INT_EQ(tape_force_read_only(volume->device), 0);
		/* Long deadline: exercise creation/stop, never a real timed tape command. */
		CHECK_INT_EQ(periodic_sync_thread_init(3600, 0, volume, NULL), 0);
		bool initialized = periodic_sync_thread_initialized(volume);
		if (initialized)
			CHECK_INT_EQ(periodic_sync_thread_destroy(volume), 0);
		ltfs_volume_free(&volume);
		if (initialized != !read_only) {
			fprintf(stderr, "periodic sync read_only=%d: initialized=%d\n", read_only, initialized);
			++failures;
		}
	}
	CHECK_INT_EQ(failures, 0);
	return 0;
}

struct mam_attribute_fixture {
	uint16_t requested_id;
	uint16_t returned_id;
	uint16_t length;
	int result;
	int calls;
};

static int read_mam_attribute_fixture(void *context, const tape_partition_t partition,
	const uint16_t id, unsigned char *buffer, const size_t size)
{
	struct mam_attribute_fixture *fixture = context;
	CHECK_INT_EQ(partition, 0);
	CHECK_INT_EQ(id, fixture->requested_id);
	CHECK_TRUE(size >= 5);
	++fixture->calls;
	if (fixture->result)
		return fixture->result;
	/* Poison following bytes: a short attribute must not copy the next descriptor. */
	memset(buffer, '!', size);
	buffer[0] = fixture->returned_id >> 8;
	buffer[1] = fixture->returned_id & 0xff;
	buffer[2] = 0x81;
	buffer[3] = fixture->length >> 8;
	buffer[4] = fixture->length & 0xff;
	for (size_t i = 0; i < fixture->length && i + 5 < size; ++i)
		buffer[i + 5] = 'A' + i % 26;
	return 0;
}

static int test_mam_volume_identifier_accepts_every_hp_length(void)
{
	/* HP LTO-6 Host Interface Guide, page145: attribute0008 length0..32. */
	struct mam_attribute_fixture fixture = {.requested_id = 0x0008, .returned_id = 0x0008};
	struct tape_ops ops = {.read_attribute = read_mam_attribute_fixture};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	int failures = 0;
	for (unsigned int length = 0; length <= 32; ++length) {
		struct tape_attr attributes;
		memset(&attributes, 0xcc, sizeof(attributes));
		fixture.length = length;
		int result = tape_get_attribute_from_cm(&device, &attributes, 0x0008);
		if (result != 0) {
			fprintf(stderr, "MAM0008 length%u rejected: %d\n", length, result);
			++failures;
			continue;
		}
		for (unsigned int i = 0; i < length; ++i)
			CHECK_INT_EQ(attributes.volume_identifier[i], 'A' + i % 26);
		CHECK_INT_EQ(attributes.volume_identifier[length], 0);
		for (unsigned int i = length + 1; i <= 32; ++i)
			CHECK_INT_EQ((unsigned char)attributes.volume_identifier[i], 0xcc);
		CHECK_INT_EQ((unsigned char)attributes.medium_serial_number[0], 0xcc);
	}
	CHECK_INT_EQ(fixture.calls, 33);
	CHECK_INT_EQ(failures, 0);
	return 0;
}

static int test_mam_volume_identifier_rejects_invalid_responses(void)
{
	struct mam_attribute_fixture fixture = {.requested_id = 0x0008, .returned_id = 0x0008};
	struct tape_ops ops = {.read_attribute = read_mam_attribute_fixture};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	for (int scenario = 0; scenario < 4; ++scenario) {
		struct tape_attr attributes, before;
		memset(&attributes, 0xcc, sizeof(attributes));
		memcpy(&before, &attributes, sizeof(before));
		fixture.returned_id = scenario == 2 ? 0x0401 : 0x0008;
		fixture.length = scenario == 0 ? 33 : scenario == 1 ? 65535 : 32;
		fixture.result = scenario == 3 ? -EIO : 0;
		CHECK_TRUE(tape_get_attribute_from_cm(&device, &attributes, 0x0008) < 0);
		CHECK_INT_EQ(memcmp(&before, &attributes, sizeof(before)), 0);
	}
	return 0;
}

static int test_other_mam_attributes_keep_exact_lengths(void)
{
	struct mam_attribute_fixture fixture = {0};
	struct tape_ops ops = {.read_attribute = read_mam_attribute_fixture};
	struct device_data device = {.backend = &ops, .backend_data = &fixture};
	const uint16_t ids[] = {0x0401, 0x0806, 0x0800};
	const uint16_t lengths[] = {32, 32, 8};
	for (size_t attribute = 0; attribute < sizeof(ids)/sizeof(ids[0]); ++attribute) {
		fixture.requested_id = fixture.returned_id = ids[attribute];
		for (int scenario = 0; scenario < 4; ++scenario) {
			struct tape_attr attributes;
			memset(&attributes, 0xcc, sizeof(attributes));
			fixture.length = scenario == 0 ? 0 : scenario == 1 ? lengths[attribute]-1
				: scenario == 2 ? lengths[attribute] : lengths[attribute]+1;
			int result = tape_get_attribute_from_cm(&device, &attributes, ids[attribute]);
			CHECK_TRUE(scenario == 2 ? result == 0 : result < 0);
		}
	}
	return 0;
}

int main(void)
{
	int readonly_failures = 0;
	CHECK_INT_EQ(test_coherency_serializes_required_nul_and_roundtrips(), 0);
	CHECK_INT_EQ(test_public_close_propagates_backend_result(), 0);
	CHECK_INT_EQ(test_unload_is_refused_without_acknowledged_close(), 0);
	CHECK_INT_EQ(test_acknowledged_close_exercises_aom_path(), 0);
	CHECK_INT_EQ(test_acknowledged_close_skips_inactive_aom_path(), 0);
	CHECK_INT_EQ(test_missing_backend_close_is_observable(), 0);
	CHECK_INT_EQ(test_writable_recovery_can_update_mam_after_write_error(), 0);
	readonly_failures |= test_forced_readonly_blocks_payload_and_filemarks();
	readonly_failures |= test_forced_readonly_blocks_both_mam_write_entries();
	readonly_failures |= test_forced_readonly_survives_load_refresh();
	readonly_failures |= test_forced_readonly_skips_high_level_coherency();
	readonly_failures |= test_forced_readonly_rejects_index_repair_before_device_io();
	readonly_failures |= test_forced_readonly_unmount_preserves_index();
	readonly_failures |= test_periodic_sync_is_not_started_for_forced_readonly();
	CHECK_INT_EQ(readonly_failures, 0);
	int mam_failures = 0;
	mam_failures |= test_mam_volume_identifier_accepts_every_hp_length();
	mam_failures |= test_mam_volume_identifier_rejects_invalid_responses();
	mam_failures |= test_other_mam_attributes_keep_exact_lengths();
	CHECK_INT_EQ(mam_failures, 0);
	return 0;
}
