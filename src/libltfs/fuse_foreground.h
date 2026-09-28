/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_FUSE_FOREGROUND_H
#define LTO_LTFS_FUSE_FOREGROUND_H

struct fuse_args;

int ltfs_force_foreground(struct fuse_args *args);

#endif /* LTO_LTFS_FUSE_FOREGROUND_H */
