/* SPDX-License-Identifier: BSD-3-Clause */

#include "file_write_state.h"
#include "perf_counters.h"

#include <errno.h>

int ltfs_file_write_state_account_release(
	const struct ltfs_file_write_state *state, int close_result,
	struct ltfs_perf_counters *counters, uint64_t identity)
{
	if (!state || !counters)
		return -EINVAL;
	if (close_result != 0 || !state->written)
		return 0;
	(void)ltfs_perf_record_file(counters, identity);
	return 1;
}
