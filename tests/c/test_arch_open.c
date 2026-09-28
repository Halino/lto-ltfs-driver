/* SPDX-License-Identifier: BSD-3-Clause */

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "libltfs/arch/ltfs_arch_ops.h"
#include "test.h"

static int check_record_reopen(mode_t mask, int share_flag,
	mode_t permission, mode_t expected_mode)
{
	char directory_template[] = "/tmp/lto-ltfs-record-XXXXXX";
	char path[sizeof(directory_template) + sizeof("/0_0_R")];
	const char payload[] = "record";
	char observed[sizeof(payload)] = { 0 };
	char *directory;
	mode_t previous_mask;
	struct stat status;
	int descriptor = -1;
	int close_result;
	int result = 1;

	directory = mkdtemp(directory_template);
	CHECK_TRUE(directory != NULL);
	CHECK_TRUE(snprintf(path, sizeof(path), "%s/0_0_R", directory) > 0);
	previous_mask = umask(mask);
	arch_open(&descriptor, path, O_RDWR | O_CREAT | O_TRUNC,
		share_flag, permission);
	umask(previous_mask);
	if (descriptor < 0)
		goto cleanup;
	if (write(descriptor, payload, sizeof(payload)) != sizeof(payload))
		goto cleanup;
	close_result = close(descriptor);
	descriptor = -1;
	if (close_result != 0)
		goto cleanup;
	if (stat(path, &status) != 0 ||
			(status.st_mode & 0777) != expected_mode)
		goto cleanup;

	arch_open(&descriptor, path, O_RDWR, SHARE_FLAG_DENYWR, 0);
	if (descriptor < 0)
		goto cleanup;
	if (read(descriptor, observed, sizeof(observed)) != sizeof(observed) ||
			memcmp(observed, payload, sizeof(payload)) != 0)
		goto cleanup;
	result = 0;

cleanup:
	if (descriptor >= 0)
		close(descriptor);
	unlink(path);
	rmdir(directory);
	return result;
}

int main(void)
{
	int failures = 0;

	puts("1..3");
	if (check_record_reopen(0022, SHARE_FLAG_DENYWR,
			PERMISSION_READWRITE, 0644) == 0)
		puts("ok 1 - normal umask creates owner-readable data record");
	else {
		puts("not ok 1 - normal umask creates owner-readable data record");
		++failures;
	}
	if (check_record_reopen(0077, SHARE_FLAG_DENYWR,
			PERMISSION_READWRITE, 0600) == 0)
		puts("ok 2 - restrictive umask keeps data record private");
	else {
		puts("not ok 2 - restrictive umask keeps data record private");
		++failures;
	}
	if (check_record_reopen(0000, SHARE_FLAG_DENYRW, 0600, 0600) == 0)
		puts("ok 3 - POSIX create mode is independent of share flags");
	else {
		puts("not ok 3 - POSIX create mode is independent of share flags");
		++failures;
	}
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
