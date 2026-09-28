/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "streaming_stats.h"

int main(void)
{
	struct ltfs_streaming_stats stats = {0};

	/* Initial idle and a completed stream with no partial producer are not backhitches. */
	ltfs_streaming_stats_wait(&stats, true);
	CHECK_INT_EQ(stats.buffer_underrun_count, 0);
	ltfs_streaming_stats_write(&stats);
	ltfs_streaming_stats_wait(&stats, false);
	CHECK_INT_EQ(stats.buffer_underrun_count, 0);

	/* One count per transition from successful streaming to empty queue. */
	ltfs_streaming_stats_write(&stats);
	ltfs_streaming_stats_write(&stats);
	ltfs_streaming_stats_wait(&stats, true);
	ltfs_streaming_stats_wait(&stats, true); /* spurious wakeup */
	CHECK_INT_EQ(stats.buffer_underrun_count, 1);
	ltfs_streaming_stats_write(&stats);
	ltfs_streaming_stats_wait(&stats, true);
	CHECK_INT_EQ(stats.buffer_underrun_count, 2);
	return 0;
}
