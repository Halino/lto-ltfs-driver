/* SPDX-License-Identifier: BSD-3-Clause */

#include "retry_policy.h"

#include <stddef.h>

#define LTFS_RETRY_ANY 0xffU
#define LTFS_RETRY_CLASS_ANY (-1)
#define LTFS_RETRY_MAX_ATTEMPTS 5U

struct retry_rule {
	int command_class;
	uint8_t sense_key;
	uint8_t asc;
	uint8_t ascq;
	enum ltfs_retry_action action;
	const char *message_code;
	bool refresh_identity;
};

static const struct retry_rule rules[] = {
	{LTFS_RETRY_CLASS_ANY, 0x01, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_RETRY_SUCCESS,
		"scsi.success.recovered", false},
	{LTFS_RETRY_CLASS_ANY, 0x02, 0x04, 0x01, LTFS_RETRY_AFTER_MS,
		"scsi.retry.becoming_ready", false},
	{LTFS_RETRY_CLASS_ANY, 0x06, 0x28, 0x00, LTFS_RETRY_AFTER_MS,
		"scsi.retry.unit_attention", true},
	{LTFS_RETRY_CLASS_ANY, 0x03, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_FAIL_PERMANENT,
		"scsi.fail.medium_error", false},
	{LTFS_RETRY_CLASS_ANY, 0x04, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_FAIL_PERMANENT,
		"scsi.fail.hardware_error", false},
	{LTFS_RETRY_CLASS_ANY, 0x05, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_FAIL_PERMANENT,
		"scsi.fail.illegal_request", false},
	{LTFS_RETRY_CLASS_ANY, 0x07, 0x27, 0x00, LTFS_FAIL_PERMANENT,
		"scsi.fail.write_protected", false},
	{LTFS_RETRY_CLASS_ANY, 0x07, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_FAIL_PERMANENT,
		"scsi.fail.data_protect", false},
	{LTFS_CMD_READ_ONLY_INFO, 0x0b, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_RETRY_AFTER_MS,
		"scsi.retry.aborted_command", false},
	{LTFS_CMD_IDEMPOTENT, 0x0b, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_RETRY_AFTER_MS,
		"scsi.retry.aborted_command", false},
	{LTFS_CMD_POSITIONING, 0x0b, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_RETRY_AFTER_MS,
		"scsi.retry.aborted_command", false},
	{LTFS_CMD_WRITE, 0x0b, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_RETRY_AFTER_MS,
		"scsi.retry.aborted_command", false},
	{LTFS_CMD_COMMIT, 0x0b, LTFS_RETRY_ANY, LTFS_RETRY_ANY, LTFS_RETRY_AFTER_MS,
		"scsi.retry.aborted_command", false},
};

static bool field_matches(uint8_t rule_value, uint8_t actual)
{
	return rule_value == LTFS_RETRY_ANY || rule_value == actual;
}

static const struct retry_rule *find_rule(const struct ltfs_retry_input *input)
{
	size_t index;
	for (index = 0; index < sizeof(rules) / sizeof(rules[0]); ++index) {
		if ((rules[index].command_class == LTFS_RETRY_CLASS_ANY ||
			rules[index].command_class == (int)input->command_class) &&
			field_matches(rules[index].sense_key, input->sense_key) &&
			field_matches(rules[index].asc, input->asc) &&
			field_matches(rules[index].ascq, input->ascq))
			return &rules[index];
	}
	return NULL;
}

static struct ltfs_retry_decision decision(enum ltfs_retry_action action,
	const char *message_code, uint32_t delay_ms, bool refresh_identity)
{
	struct ltfs_retry_decision result;
	result.action = action;
	result.message_code = message_code;
	result.delay_ms = delay_ms;
	result.refresh_identity = refresh_identity;
	return result;
}

static struct ltfs_retry_decision retry_decision(
	const struct ltfs_retry_input *input, const struct retry_rule *rule)
{
	static const uint32_t backoff_ms[] = {250, 500, 1000, 2000, 4000};
	uint64_t remaining;
	uint32_t delay;

	if (rule->refresh_identity && input->unit_attention_seen)
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.unit_attention_repeated", 0, false);
	if (input->attempt >= LTFS_RETRY_MAX_ATTEMPTS ||
		input->elapsed_ms >= input->deadline_ms)
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.retry_deadline", 0, false);
	remaining = input->deadline_ms - input->elapsed_ms;
	delay = backoff_ms[input->attempt];
	if ((uint64_t)delay > remaining)
		delay = (uint32_t)remaining;
	if (delay == 0)
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.retry_deadline", 0, false);
	return decision(LTFS_RETRY_AFTER_MS, rule->message_code, delay,
		rule->refresh_identity);
}

static const char *ambiguous_message(
	enum ltfs_retry_command_class command_class)
{
	switch (command_class) {
		case LTFS_CMD_RELATIVE_POSITIONING:
			return "scsi.stop.ambiguous_position";
		case LTFS_CMD_STATE_CHANGING:
		case LTFS_CMD_DESTRUCTIVE:
		case LTFS_CMD_UNKNOWN:
			return "scsi.stop.ambiguous_state_change";
		default:
			return "scsi.stop.ambiguous_write";
	}
}

struct ltfs_retry_decision ltfs_retry_classify(
	const struct ltfs_retry_input *input)
{
	const struct retry_rule *rule;
	if (!input || input->command_class < LTFS_CMD_READ_ONLY_INFO ||
		input->command_class > LTFS_CMD_UNKNOWN ||
		input->sense_key > 0x0f)
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.invalid_retry_input", 0, false);

	rule = find_rule(input);
	if (rule && rule->action == LTFS_RETRY_SUCCESS) {
		if (input->recovered_result_is_success)
			return decision(rule->action, rule->message_code, 0, false);
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.recovered_status_preserved", 0, false);
	}

	if (input->completion_may_be_ambiguous)
		return decision(LTFS_STOP_AMBIGUOUS,
			ambiguous_message(input->command_class), 0, false);

	if (!rule)
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.unknown_sense", 0, false);
	if (rule->action == LTFS_RETRY_AFTER_MS &&
		(input->command_class == LTFS_CMD_RELATIVE_POSITIONING ||
		input->command_class == LTFS_CMD_STATE_CHANGING ||
		input->command_class == LTFS_CMD_DESTRUCTIVE ||
		input->command_class == LTFS_CMD_UNKNOWN))
		return decision(LTFS_FAIL_PERMANENT,
			"scsi.fail.non_idempotent_retry_blocked", 0, false);
	if (rule->action == LTFS_RETRY_AFTER_MS)
		return retry_decision(input, rule);
	return decision(rule->action, rule->message_code, 0, false);
}
