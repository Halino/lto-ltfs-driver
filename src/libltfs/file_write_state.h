/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTFS_FILE_WRITE_STATE_H
#define LTFS_FILE_WRITE_STATE_H

#include <stdbool.h>
#include <stdint.h>

struct ltfs_perf_counters;

struct ltfs_file_write_state {
	bool dirty;
	bool written;
};

static inline void ltfs_file_write_state_record_write(
	struct ltfs_file_write_state *state, int write_result)
{
	if (write_result != 0)
		return;
	state->dirty = true;
	state->written = true;
}

static inline void ltfs_file_write_state_mark_synced(
	struct ltfs_file_write_state *state)
{
	state->dirty = false;
}

static inline bool ltfs_file_write_state_needs_flush(
	const struct ltfs_file_write_state *state)
{
	return state->dirty;
}

static inline bool ltfs_file_write_state_was_written(
	const struct ltfs_file_write_state *state)
{
	return state->written;
}

/* Return 1 when release accounting ran, 0 when ineligible, or -EINVAL. */
int ltfs_file_write_state_account_release(
	const struct ltfs_file_write_state *state, int close_result,
	struct ltfs_perf_counters *counters, uint64_t identity);

#endif /* LTFS_FILE_WRITE_STATE_H */
