/* SPDX-License-Identifier: BSD-3-Clause */

#include "fuse_foreground.h"

#include <errno.h>
#include <fuse.h>
#include <string.h>

int ltfs_force_foreground(struct fuse_args *args)
{
	int i;
	if (!args || args->argc < 1 || !args->argv)
		return -EINVAL;
	for (i = 0; i < args->argc; ++i)
		if (args->argv[i] && !strcmp(args->argv[i], "-f"))
			return 0;
	return fuse_opt_insert_arg(args, 1, "-f");
}
