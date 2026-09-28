/* SPDX-License-Identifier: BSD-3-Clause */

#include "streaming_stats.h"

void ltfs_streaming_stats_write(struct ltfs_streaming_stats *stats)
{
	if (stats)
		stats->write_since_wait = true;
}

void ltfs_streaming_stats_wait(struct ltfs_streaming_stats *stats,
	bool stream_expected)
{
	if (!stats)
		return;
	if (stream_expected && stats->write_since_wait &&
		stats->buffer_underrun_count != UINT64_MAX)
		++stats->buffer_underrun_count;
	stats->write_since_wait = false;
}
