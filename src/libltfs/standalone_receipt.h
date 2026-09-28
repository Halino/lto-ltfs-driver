/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTFS_STANDALONE_RECEIPT_H
#define LTFS_STANDALONE_RECEIPT_H

#include "finalization.h"

#define LTFS_STANDALONE_IDENTITY_TEXT_MAX 256

struct ltfs_standalone_ready_identity {
	const char *operation_id;
	const char *volume_uuid;
	uint64_t prior_generation;
	bool read_only;
	const char *drive_serial;
	bool mam_barcode_valid;
	const char *mam_barcode;
	bool mam_volume_serial_valid;
	const char *mam_volume_serial;
	bool ltfs_volume_label_valid;
	const char *ltfs_volume_label;
};

int ltfs_standalone_receipt_ready(const char *path,
	const struct ltfs_standalone_ready_identity *identity);
int ltfs_standalone_receipt_persist(const char *path,
	const struct ltfs_standalone_ready_identity *identity,
	const struct ltfs_commit_receipt *receipt,
	struct ltfs_commit_ack *ack);
int ltfs_standalone_receipt_finalize(const char *path,
	const struct ltfs_standalone_ready_identity *identity,
	const struct ltfs_commit_receipt *prepared_receipt,
	const struct ltfs_commit_receipt *terminal_receipt);

#endif
