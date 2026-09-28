/* SPDX-License-Identifier: LGPL-2.1-only */
/* Downstream modifications: 2026-08 through 2026-09; see LGPL-NOTICE. */

#ifndef LTO_LTFS_INFO_H
#define LTO_LTFS_INFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define LTFS_INFO_TEXT_MAX 256
#define LTFS_INFO_UUID_SIZE 37
#define LTFS_INFO_SHA256_HEX_SIZE 65

enum ltfs_info_exit {
	LTFS_INFO_READY = 0,
	LTFS_INFO_NO_MEDIA = 3,
	LTFS_INFO_IDENTITY_MISMATCH = 4,
	LTFS_INFO_UNSUPPORTED_MEDIA = 5,
	LTFS_INFO_DEVICE_BUSY = 6,
	LTFS_INFO_READ_FAILURE = 7,
};

enum ltfs_info_backend_result {
	LTFS_INFO_BACKEND_OK = 0,
	LTFS_INFO_BACKEND_NO_MEDIA = -1,
	LTFS_INFO_BACKEND_UNSUPPORTED = -2,
	LTFS_INFO_BACKEND_BUSY = -3,
	LTFS_INFO_BACKEND_READ_ERROR = -4,
};

enum ltfs_info_mode {
	LTFS_INFO_MODE_UNMOUNTED = 0,
	LTFS_INFO_MODE_PRE_FORMAT = 1,
};

enum ltfs_info_media_state {
	LTFS_INFO_MEDIA_UNSET = 0,
	LTFS_INFO_MEDIA_LTFS,
	LTFS_INFO_MEDIA_UNIDENTIFIED,
};

struct ltfs_info_identity {
	char target_sha256[LTFS_INFO_SHA256_HEX_SIZE];
	char drive_serial[LTFS_INFO_TEXT_MAX];
	char drive_wwid[LTFS_INFO_TEXT_MAX];
	char scsi_tuple[LTFS_INFO_TEXT_MAX];
};

struct ltfs_info_capacity {
	unsigned char log_page;
	unsigned int capacity_offset;
	uint64_t remaining_partition0_mib;
	uint64_t remaining_partition1_mib;
	uint64_t maximum_partition0_mib;
	uint64_t maximum_partition1_mib;
};

/* An explicit expectation, never filled from the just-observed medium. */
struct ltfs_info_expected_medium {
	const char *drive_serial;
	const char *medium_serial;
	const char *volume_label;
	const char *volume_uuid;
	uint64_t index_generation;
};

struct ltfs_info_capacity_report;

struct ltfs_info_record {
	char tape_by_id[LTFS_INFO_TEXT_MAX];
	char scsi_by_id[LTFS_INFO_TEXT_MAX];
	char build_name[LTFS_INFO_TEXT_MAX];
	char build_version[LTFS_INFO_TEXT_MAX];
	struct ltfs_info_identity identity;
	enum ltfs_info_media_state media_state;
	bool ready;
	bool mam_application_name_valid;
	char mam_application_name[LTFS_INFO_TEXT_MAX];
	bool mam_application_version_valid;
	char mam_application_version[LTFS_INFO_TEXT_MAX];
	bool mam_volume_label_valid;
	char mam_volume_label[LTFS_INFO_TEXT_MAX];
	bool mam_barcode_valid;
	char mam_barcode[LTFS_INFO_TEXT_MAX];
	bool mam_volume_serial_valid;
	char mam_volume_serial[LTFS_INFO_TEXT_MAX];
	bool capacity_valid;
	struct ltfs_info_capacity capacity;
	bool volume_uuid_valid;
	char volume_uuid[LTFS_INFO_UUID_SIZE];
	bool index_generation_valid;
	uint64_t index_generation;
};

struct ltfs_info_backend {
	bool read_only_guaranteed;
	void *context;
	int (*open)(void *context, const char *sg_path);
	int (*close)(void *context);
	int (*inquiry)(void *context);
	int (*test_unit_ready)(void *context);
	int (*read_text_attribute)(void *context, uint16_t attribute,
		char *value, size_t size);
	int (*read_coherency)(void *context, char uuid[LTFS_INFO_UUID_SIZE],
		uint64_t *generation);
	int (*remaining_capacity)(void *context,
		struct ltfs_info_capacity *capacity);
};

struct ltfs_info_capacity_report {
	struct ltfs_info_record record;
	bool identity_verified_before_after;
};

int ltfs_info_collect_capacity(const char *sg_path,
	const struct ltfs_info_identity *identity, struct ltfs_info_backend *backend,
	const struct ltfs_info_expected_medium *expected,
	struct ltfs_info_capacity_report *report);
int ltfs_info_write_capacity_json(FILE *stream,
	const struct ltfs_info_capacity_report *report);
int ltfs_info_select_capacity_page(const unsigned char *inquiry, size_t size,
	unsigned char *page);

int ltfs_info_collect(const char *sg_path,
	const struct ltfs_info_identity *identity,
	struct ltfs_info_backend *backend, struct ltfs_info_record *record);
int ltfs_info_collect_mode(const char *sg_path,
	const struct ltfs_info_identity *identity,
	struct ltfs_info_backend *backend, enum ltfs_info_mode mode,
	struct ltfs_info_record *record);
int ltfs_info_set_config_paths(struct ltfs_info_record *record,
	const char *tape_by_id, const char *scsi_by_id);
int ltfs_info_write_json(FILE *stream, const struct ltfs_info_record *record);
int ltfs_info_write_device_config_json(FILE *stream, const char *nst_path,
	const char *sg_path, const struct ltfs_info_identity *identity);
int ltfs_info_run_self_test_fixture(FILE *stream, const char *name);
bool ltfs_info_opcode_allowed(uint8_t opcode);
int ltfs_info_parse_coherency(const unsigned char *raw, size_t size,
	char uuid[LTFS_INFO_UUID_SIZE], uint64_t *generation);
int ltfs_info_parse_capacity_page17(const unsigned char *page, size_t size,
	struct ltfs_info_capacity *capacity);
int ltfs_info_extract_attribute_response(const unsigned char *response,
	size_t response_size, uint16_t attribute, unsigned char *output,
	size_t output_size);
int ltfs_info_parse_text_attribute_descriptor(const unsigned char *descriptor,
	size_t descriptor_size, uint16_t attribute, char *output,
	size_t output_size);
int ltfs_info_build_capacity_log_sense_cdb(unsigned char cdb[10],
	size_t allocation_size);

#endif /* LTO_LTFS_INFO_H */
