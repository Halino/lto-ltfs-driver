/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_RETRY_POLICY_H
#define LTO_LTFS_RETRY_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum ltfs_retry_command_class {
	LTFS_CMD_READ_ONLY_INFO = 0,
	LTFS_CMD_IDEMPOTENT,
	LTFS_CMD_POSITIONING,
	LTFS_CMD_WRITE,
	LTFS_CMD_COMMIT,
	LTFS_CMD_DESTRUCTIVE,
	LTFS_CMD_RELATIVE_POSITIONING,
	LTFS_CMD_STATE_CHANGING,
	LTFS_CMD_UNKNOWN,
};

enum ltfs_retry_action {
	LTFS_RETRY_SUCCESS = 0,
	LTFS_RETRY_AFTER_MS,
	LTFS_FAIL_PERMANENT,
	LTFS_STOP_AMBIGUOUS,
};

struct ltfs_retry_input {
	enum ltfs_retry_command_class command_class;
	uint8_t sense_key;
	uint8_t asc;
	uint8_t ascq;
	unsigned int attempt;
	uint64_t elapsed_ms;
	uint64_t deadline_ms;
	bool completion_may_be_ambiguous;
	bool unit_attention_seen;
	bool recovered_result_is_success; /**< Result normalized by upstream sense map. */
};

struct ltfs_retry_decision {
	enum ltfs_retry_action action;
	const char *message_code;
	uint32_t delay_ms;
	bool refresh_identity;
};

struct ltfs_retry_decision ltfs_retry_classify(
	const struct ltfs_retry_input *input);

#endif /* LTO_LTFS_RETRY_POLICY_H */
