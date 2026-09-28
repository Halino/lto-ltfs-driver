/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_TEST_FAKE_LTFS_H
#define LTO_LTFS_TEST_FAKE_LTFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#define LTFS_NULL_ARG 1000
#define LTFS_ERR 0
#define LTFS_INFO 2
#define LTFS_COPYRIGHT_0 "copyright"
#define LTFS_COPYRIGHT_1 "copyright"
#define LTFS_COPYRIGHT_2 "copyright"
#define LTFS_COPYRIGHT_3 "copyright"
#define LTFS_COPYRIGHT_4 "copyright"
#define LTFS_COPYRIGHT_5 "copyright"

#define ltfsmsg(...) do { } while (0)
#define CHECK_ARG_NULL(value, result) \
	do { if (!(value)) return (result); } while (0)

typedef int ltfs_mutex_t;

struct ltfs_volume { int unused; };
struct dentry { uint64_t size; };

int ltfs_mutex_init(ltfs_mutex_t *mutex);
void ltfs_mutex_destroy(ltfs_mutex_t *mutex);
char ltfs_dp_id(struct ltfs_volume *volume);

#endif
