/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "file_write_state.h"
#include "perf_counters.h"
#include "standalone_receipt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
	char dir[] = "/tmp/ltfs-fsync-receipt-test-XXXXXX";
	char path[512];
	char payload[8192];
	struct ltfs_file_write_state state = {0};
	struct ltfs_perf_counters counters;
	struct ltfs_perf_config config = {
		.memory_ceiling_bytes = LTFS_PERF_MAX_MEMORY_CEILING,
	};
	struct ltfs_perf_snapshot snapshot;
	struct ltfs_commit_receipt receipt = {0};
	struct ltfs_commit_receipt prepared;
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
	bool emit_due = false;
	int fd;
	ssize_t got;

	CHECK_INT_EQ(ltfs_perf_init(&counters, &config), 0);
	ltfs_file_write_state_record_write(&state, 0);
	CHECK_TRUE(ltfs_file_write_state_needs_flush(&state));
	CHECK_TRUE(ltfs_file_write_state_was_written(&state));
	CHECK_INT_EQ(ltfs_perf_record_write(&counters, 1234, 0, 0,
		&emit_due), 0);

	/* A successful fsync clears flush state, not operation file identity. */
	ltfs_file_write_state_mark_synced(&state);
	CHECK_TRUE(!ltfs_file_write_state_needs_flush(&state));
	CHECK_TRUE(ltfs_file_write_state_was_written(&state));
	CHECK_INT_EQ(ltfs_file_write_state_account_release(&state, -EIO,
		&counters, 77), 0);
	CHECK_INT_EQ(ltfs_perf_set_finalizing(&counters, true), 0);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 60, &snapshot), 0);
	CHECK_INT_EQ(snapshot.total_files, 0);
	CHECK_INT_EQ(ltfs_perf_set_finalizing(&counters, false), 0);

	CHECK_INT_EQ(ltfs_file_write_state_account_release(&state, 0,
		&counters, 77), 1);
	/* A second successful handle for the same file remains deduplicated. */
	{
		struct ltfs_file_write_state duplicate = {0};
		ltfs_file_write_state_record_write(&duplicate, 0);
		CHECK_INT_EQ(ltfs_file_write_state_account_release(&duplicate, 0,
			&counters, 77), 1);
	}
	/* A handle with no successful write does not create a file count. */
	{
		struct ltfs_file_write_state unwritten = {0};
		CHECK_INT_EQ(ltfs_file_write_state_account_release(&unwritten, 0,
			&counters, 88), 0);
	}
	/* A failed backend write never marks the handle as written. */
	{
		struct ltfs_file_write_state failed_write = {0};
		ltfs_file_write_state_record_write(&failed_write, -EIO);
		CHECK_INT_EQ(ltfs_file_write_state_account_release(&failed_write, 0,
			&counters, 99), 0);
	}

	CHECK_INT_EQ(ltfs_perf_set_finalizing(&counters, true), 0);
	CHECK_INT_EQ(ltfs_perf_snapshot(&counters, 60, &snapshot), 0);
	CHECK_TRUE(snapshot.total_files_valid);
	CHECK_INT_EQ(snapshot.total_files, 1);
	CHECK_INT_EQ(snapshot.total_bytes, 1234);

	CHECK_TRUE(mkdtemp(dir) != NULL);
	snprintf(path, sizeof(path), "%s/receipt.json", dir);
	strcpy(receipt.operation_id, "11111111-1111-4111-8111-111111111111");
	strcpy(receipt.volume_uuid, "22222222-2222-4222-8222-222222222222");
	receipt.prior_generation = 7;
	receipt.new_generation = 8;
	receipt.bytes_valid = true;
	receipt.bytes = snapshot.total_bytes;
	receipt.files_valid = snapshot.total_files_valid;
	receipt.files = snapshot.total_files;
	receipt.media_committed = true;
	identity.operation_id = receipt.operation_id;
	identity.volume_uuid = receipt.volume_uuid;
	identity.prior_generation = receipt.prior_generation;
	CHECK_INT_EQ(ltfs_standalone_receipt_ready(path, &identity), 0);
	CHECK_INT_EQ(ltfs_standalone_receipt_persist(path, &identity, &receipt,
		&ack), 0);
	prepared = receipt;
	receipt.catalog_acknowledged = true;
	receipt.device_close_result_valid = true;
	CHECK_INT_EQ(ltfs_standalone_receipt_finalize(path, &identity, &prepared,
		&receipt), 0);

	fd = open(path, O_RDONLY);
	CHECK_TRUE(fd >= 0);
	got = read(fd, payload, sizeof(payload) - 1);
	CHECK_TRUE(got > 0);
	payload[got] = '\0';
	close(fd);
	CHECK_TRUE(strstr(payload,
		"\"bytes_valid\":true,\"bytes\":1234") != NULL);
	CHECK_TRUE(strstr(payload,
		"\"files_valid\":true,\"files\":1") != NULL);

	ltfs_perf_destroy(&counters);
	CHECK_INT_EQ(unlink(path), 0);
	CHECK_INT_EQ(rmdir(dir), 0);
	return 0;
}
