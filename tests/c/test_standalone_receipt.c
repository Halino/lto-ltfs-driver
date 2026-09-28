/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "standalone_receipt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	char dir[] = "/tmp/ltfs-receipt-test-XXXXXX";
	char path[512];
	char pending_path[520];
	char ready_path[520];
	char payload[8192];
	const char ready_expected[] =
		"{\"schema\":1,\"stage\":\"ready\","
		"\"operation_id\":\"11111111-1111-4111-8111-111111111111\","
		"\"volume_uuid\":\"22222222-2222-4222-8222-222222222222\","
		"\"prior_generation\":7,\"read_only\":false,"
		"\"drive_serial\":\"DRIVE-TEST-01\","
		"\"mam_barcode\":\"TEST01\","
		"\"mam_volume_serial\":\"SERIAL-TEST-01\","
		"\"ltfs_volume_label\":\"TEST VOLUME\"}\n";
	struct ltfs_commit_receipt receipt = {0};
	struct ltfs_commit_ack ack = {0};
	struct ltfs_standalone_ready_identity identity = {
		.drive_serial = "DRIVE-TEST-01",
		.mam_barcode_valid = true,
		.mam_barcode = "TEST01",
		.mam_volume_serial_valid = true,
		.mam_volume_serial = "SERIAL-TEST-01",
		.ltfs_volume_label_valid = true,
		.ltfs_volume_label = "TEST VOLUME",
	};
	struct stat st;
	int fd;
	ssize_t got;

	CHECK_TRUE(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/receipt.json", dir);
	snprintf(pending_path, sizeof(pending_path), "%s.pending", path);
	snprintf(ready_path, sizeof(ready_path), "%s.ready", path);
	strcpy(receipt.operation_id, "11111111-1111-4111-8111-111111111111");
	strcpy(receipt.volume_uuid, "22222222-2222-4222-8222-222222222222");
	receipt.prior_generation = 7;
	receipt.new_generation = 8;
	receipt.bytes_valid = true;
	receipt.bytes = 1234;
	receipt.files_valid = true;
	receipt.files = 4;
	receipt.media_committed = true;
	identity.operation_id = receipt.operation_id;
	identity.volume_uuid = receipt.volume_uuid;
	identity.prior_generation = receipt.prior_generation;
	{
		struct ltfs_standalone_ready_identity incomplete = identity;
		incomplete.mam_volume_serial_valid = false;
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &incomplete),
			-EINVAL);
	}
	{
		struct ltfs_standalone_ready_identity invalid_text = identity;
		invalid_text.mam_barcode = "TEST\00101";
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &invalid_text),
			-EINVAL);
		invalid_text.mam_barcode = " TEST01";
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &invalid_text),
			-EINVAL);
	}
	{
		struct ltfs_standalone_ready_identity uppercase_uuid = identity;
		uppercase_uuid.volume_uuid =
			"abcdefab-cdef-4abc-8abc-abcdefabcdeF";
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &uppercase_uuid),
			-EINVAL);
	}
	{
		char invalid_path[PATH_MAX];
		snprintf(invalid_path, sizeof(invalid_path), "%s/.", dir);
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(invalid_path,
			&identity), -EINVAL);
		snprintf(invalid_path, sizeof(invalid_path), "%s/..", dir);
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(invalid_path,
			&identity), -EINVAL);
	}
	CHECK_INT_EQ(chmod(dir, 0750), 0);
	CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &identity), -EPERM);
	CHECK_INT_EQ(chmod(dir, 0700), 0);
	CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &identity), 0);
	fd = open(ready_path, O_RDONLY);
	CHECK_TRUE(fd >= 0);
	got = read(fd, payload, sizeof(payload) - 1);
	CHECK_TRUE(got > 0);
	payload[got] = '\0';
	close(fd);
	CHECK_STR_EQ(payload, ready_expected);
	if (geteuid() == 0) {
		gid_t foreign_gid = getegid() == 1 ? 2 : 1;
		CHECK_INT_EQ(chown(ready_path, (uid_t)-1, foreign_gid), 0);
		CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity,
			&receipt, &ack), -EPERM);
		CHECK_INT_EQ(chown(ready_path, (uid_t)-1, getegid()), 0);
	}
	CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &identity), -EEXIST);
	fd = open(ready_path, O_WRONLY | O_TRUNC);
	CHECK_TRUE(fd >= 0);
	CHECK_INT_EQ(write(fd, "mutated\n", 8), 8);
	CHECK_INT_EQ(fsync(fd), 0);
	CHECK_INT_EQ(close(fd), 0);
	CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity, &receipt, &ack),
		-EPROTO);
	fd = open(ready_path, O_WRONLY | O_TRUNC);
	CHECK_TRUE(fd >= 0);
	CHECK_INT_EQ(write(fd, ready_expected, strlen(ready_expected)),
		strlen(ready_expected));
	CHECK_INT_EQ(fsync(fd), 0);
	CHECK_INT_EQ(close(fd), 0);

	CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity, &receipt,
		&ack), 0);
	CHECK_TRUE(ack.durable);
	CHECK_INT_EQ(ack.generation, 8);
	CHECK_TRUE(!strcmp(ack.operation_id, receipt.operation_id));
	CHECK_TRUE(!strcmp(ack.volume_uuid, receipt.volume_uuid));
	CHECK_TRUE(stat(path, &st) < 0 && errno == ENOENT);
	CHECK_INT_EQ(stat(pending_path, &st), 0);
	CHECK_INT_EQ(st.st_mode & 0777, 0600);
	fd = open(pending_path, O_RDONLY);
	CHECK_TRUE(fd >= 0);
	got = read(fd, payload, sizeof(payload) - 1);
	CHECK_TRUE(got > 0);
	payload[got] = '\0';
	close(fd);
	CHECK_STR_EQ(payload,
		"{\"schema\":1,\"stage\":\"prepared\","
		"\"operation_id\":\"11111111-1111-4111-8111-111111111111\","
		"\"volume_uuid\":\"22222222-2222-4222-8222-222222222222\","
		"\"prior_generation\":7,\"new_generation\":8,"
		"\"bytes_valid\":true,\"bytes\":1234,"
		"\"files_valid\":true,\"files\":4,"
		"\"phase_duration_ns\":[0,0,0,0,0,0,0,0,0,0,0],"
		"\"capture_duration_ns\":0,\"device_close_duration_ns\":0,"
		"\"device_close_result_valid\":false,\"device_close_result\":0,"
		"\"catalog_ack_duration_ns\":0,\"media_committed\":true,"
		"\"catalog_acknowledged\":false,\"cleanup_failed\":false,"
		"\"result\":0}\n");

	{
		struct ltfs_commit_receipt prepared = receipt;
		receipt.catalog_acknowledged = true;
		receipt.device_close_result_valid = true;
		receipt.device_close_result = 0;
		receipt.result = 0;
		{
			struct ltfs_commit_receipt wrong = receipt;
			strcpy(wrong.operation_id,
				"33333333-3333-4333-8333-333333333333");
			CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path, &identity, &prepared,
				&wrong), -EINVAL);
			CHECK_INT_EQ(stat(ready_path, &st), 0);
			CHECK_INT_EQ(stat(pending_path, &st), 0);
		}
		{
			struct ltfs_standalone_ready_identity wrong_identity = identity;
			wrong_identity.mam_barcode = "OTHER1";
			CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path,
				&wrong_identity, &prepared, &receipt), -EPROTO);
			CHECK_INT_EQ(stat(ready_path, &st), 0);
			CHECK_INT_EQ(stat(pending_path, &st), 0);
		}
		{
			struct ltfs_standalone_ready_identity wrong_identity = identity;
			wrong_identity.ltfs_volume_label = "OTHER VOLUME";
			CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path,
				&wrong_identity, &prepared, &receipt), -EPROTO);
			CHECK_INT_EQ(stat(ready_path, &st), 0);
			CHECK_INT_EQ(stat(pending_path, &st), 0);
		}
		fd = open(pending_path, O_WRONLY | O_TRUNC);
		CHECK_TRUE(fd >= 0);
		CHECK_INT_EQ(write(fd, "mutated\n", 8), 8);
		CHECK_INT_EQ(fsync(fd), 0);
		CHECK_INT_EQ(close(fd), 0);
		CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path, &identity, &prepared,
			&receipt), -EPROTO);
		fd = open(pending_path, O_WRONLY | O_TRUNC);
		CHECK_TRUE(fd >= 0);
		CHECK_INT_EQ(write(fd, payload, strlen(payload)), strlen(payload));
		CHECK_INT_EQ(fsync(fd), 0);
		CHECK_INT_EQ(close(fd), 0);
		CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path, &identity, &prepared,
			&receipt), 0);
	CHECK_TRUE(stat(pending_path, &st) < 0 && errno == ENOENT);
	CHECK_TRUE(stat(ready_path, &st) < 0 && errno == ENOENT);
	CHECK_INT_EQ(stat(path, &st), 0);
	memset(payload, 0, sizeof(payload));
	fd = open(path, O_RDONLY);
	CHECK_TRUE(fd >= 0);
	got = read(fd, payload, sizeof(payload) - 1);
	CHECK_TRUE(got > 0);
	payload[got] = '\0';
	close(fd);
	CHECK_STR_EQ(payload,
		"{\"schema\":1,\"stage\":\"terminal\","
		"\"operation_id\":\"11111111-1111-4111-8111-111111111111\","
		"\"volume_uuid\":\"22222222-2222-4222-8222-222222222222\","
		"\"prior_generation\":7,\"new_generation\":8,"
		"\"bytes_valid\":true,\"bytes\":1234,"
		"\"files_valid\":true,\"files\":4,"
		"\"phase_duration_ns\":[0,0,0,0,0,0,0,0,0,0,0],"
		"\"capture_duration_ns\":0,\"device_close_duration_ns\":0,"
		"\"device_close_result_valid\":true,\"device_close_result\":0,"
		"\"catalog_ack_duration_ns\":0,\"media_committed\":true,"
		"\"catalog_acknowledged\":true,\"cleanup_failed\":false,"
		"\"result\":0}\n");

	/* Never overwrite an existing receipt or follow a caller-selected relative path. */
		receipt.catalog_acknowledged = false;
		receipt.device_close_result_valid = false;
		CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity, &receipt, &ack),
			-EEXIST);
		receipt.catalog_acknowledged = true;
		receipt.device_close_result_valid = true;
		CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path, &identity, &prepared,
			&receipt), -EEXIST);
	}
	CHECK_INT_EQ(ltfs_standalone_receipt_persist("receipt.json", &identity,
		&receipt, &ack), -EINVAL);
	CHECK_INT_EQ(unlink(path), 0);
	/* A read-only session seals and preserves the observed generation. */
	snprintf(path, sizeof(path), "%s/read-only.json", dir);
	snprintf(ready_path, sizeof(ready_path), "%s.ready", path);
	memset(&receipt, 0, sizeof(receipt));
	strcpy(receipt.operation_id,
		"44444444-4444-4444-8444-444444444444");
	strcpy(receipt.volume_uuid,
		"55555555-5555-4555-8555-555555555555");
	receipt.prior_generation = 8;
	receipt.new_generation = 8;
	receipt.bytes_valid = true;
	receipt.files_valid = true;
	receipt.media_committed = true;
	identity.operation_id = receipt.operation_id;
	identity.volume_uuid = receipt.volume_uuid;
	identity.prior_generation = receipt.prior_generation;
	identity.read_only = true;
	{
		struct ltfs_commit_receipt invalid_read_only = receipt;
		invalid_read_only.new_generation = 9;
		CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &identity), 0);
		CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity,
			&invalid_read_only, &ack), -EINVAL);
		CHECK_INT_EQ(unlinkat(AT_FDCWD, ready_path, 0), 0);
	}
	CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &identity), 0);
	CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity, &receipt,
		&ack), 0);
	{
		struct ltfs_commit_receipt prepared = receipt;
		receipt.catalog_acknowledged = true;
		receipt.device_close_result_valid = true;
		CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path, &identity, &prepared,
			&receipt), 0);
	}
	fd = open(path, O_RDONLY);
	CHECK_TRUE(fd >= 0);
	got = read(fd, payload, sizeof(payload) - 1);
	CHECK_TRUE(got > 0);
	payload[got] = '\0';
	close(fd);
	CHECK_TRUE(strstr(payload,
		"\"prior_generation\":8,\"new_generation\":8") != NULL);
	CHECK_INT_EQ(unlink(path), 0);
	CHECK_INT_EQ(rmdir(dir), 0);
	return 0;
}
