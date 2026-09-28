/* SPDX-License-Identifier: BSD-3-Clause */

#include <stdint.h>

#include "libltfs/arch/time_internal.h"

#if !defined(__APPLE__) && !defined(mingw_PLATFORM)
int get_unix_current_timespec(struct ltfs_timespec *now)
{
	if (!now)
		return -1;
	now->tv_sec = 0;
	now->tv_nsec = 0;
	return 0;
}
#endif
