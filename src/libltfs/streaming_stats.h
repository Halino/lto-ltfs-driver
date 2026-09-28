/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTFS_STREAMING_STATS_H
#define LTFS_STREAMING_STATS_H

#include <stdbool.h>
#include <stdint.h>

struct ltfs_streaming_stats {
	bool write_since_wait;
	uint64_t buffer_underrun_count;
};

void ltfs_streaming_stats_write(struct ltfs_streaming_stats *stats);
void ltfs_streaming_stats_wait(struct ltfs_streaming_stats *stats,
	bool stream_expected);

#endif
