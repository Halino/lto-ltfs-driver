/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_TEST_FAKE_FUSE_H
#define LTO_LTFS_TEST_FAKE_FUSE_H

struct fuse_args {
	int argc;
	char **argv;
	int allocated;
};

#define FUSE_ARGS_INIT(argc_value, argv_value) \
	{ (argc_value), (argv_value), 0 }

int fuse_opt_insert_arg(struct fuse_args *args, int position,
	const char *argument);

#endif /* LTO_LTFS_TEST_FAKE_FUSE_H */
