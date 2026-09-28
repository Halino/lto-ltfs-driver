/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include "libltfs/finalization.h"
#include "libltfs/ltfs.h"
#include "test.h"

int fcfs_flush(struct dentry *d, bool closeflag, void *iosched_handle);
char iosched_fcfs_dat[] = "";

int ltfs_mutex_init(ltfs_mutex_t *mutex)
{
	(void)mutex;
	return 0;
}

void ltfs_mutex_destroy(ltfs_mutex_t *mutex)
{
	(void)mutex;
}

char ltfs_dp_id(struct ltfs_volume *volume)
{
	(void)volume;
	return 0;
}

int ltfs_fsraw_open(const char *path, bool open_write, struct dentry **dentry,
	struct ltfs_volume *volume)
{
	(void)path;
	(void)open_write;
	(void)dentry;
	(void)volume;
	return 0;
}

int ltfs_fsraw_close(struct dentry *dentry)
{
	(void)dentry;
	return 0;
}

ssize_t ltfs_fsraw_read(struct dentry *dentry, char *buffer, size_t size,
	off_t offset, struct ltfs_volume *volume)
{
	(void)dentry;
	(void)buffer;
	(void)size;
	(void)offset;
	(void)volume;
	return 0;
}

ssize_t ltfs_fsraw_write(struct dentry *dentry, const char *buffer, size_t size,
	off_t offset, int partition, bool use_scheduler,
	struct ltfs_volume *volume)
{
	(void)dentry;
	(void)buffer;
	(void)size;
	(void)offset;
	(void)partition;
	(void)use_scheduler;
	(void)volume;
	return 0;
}

int ltfs_fsraw_truncate(struct dentry *dentry, off_t length,
	struct ltfs_volume *volume)
{
	(void)dentry;
	(void)length;
	(void)volume;
	return 0;
}

struct fixture {
	int scheduler_handle;
	int flush_calls;
};

static int flush_all(void *context)
{
	struct fixture *fixture = context;

	++fixture->flush_calls;
	return fcfs_flush(NULL, true, &fixture->scheduler_handle);
}

static int fail_commit_after_flush(void *context,
	struct ltfs_finalization *finalization)
{
	(void)context;
	(void)finalization;
	return -EIO;
}

static int close_device(void *context)
{
	(void)context;
	return 0;
}

int main(void)
{
	struct ltfs_finalization_config config = { 0 };
	struct ltfs_finalization finalization;
	struct ltfs_commit_receipt receipt;
	struct fixture fixture = { 0 };

	config.context = &fixture;
	config.operation_id = "11111111-1111-4111-8111-111111111111";
	config.volume_uuid = "22222222-2222-4222-8222-222222222222";
	config.ops.flush_data = flush_all;
	config.ops.commit_unmount = fail_commit_after_flush;
	config.ops.close_device = close_device;
	CHECK_INT_EQ(ltfs_finalization_init(&finalization, &config), 0);
	CHECK_INT_EQ(ltfs_finalize(&finalization, &receipt), -EIO);
	CHECK_INT_EQ(fixture.flush_calls, 1);
	CHECK_INT_EQ(receipt.result, -EIO);
	CHECK_TRUE(!receipt.cleanup_failed);
	CHECK_INT_EQ(fcfs_flush(NULL, false, NULL), -LTFS_NULL_ARG);
	return EXIT_SUCCESS;
}
