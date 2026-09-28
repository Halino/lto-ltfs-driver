/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

#define OUTPUT_CAPACITY 256

struct command_result {
	int exit_code;
	char standard_output[OUTPUT_CAPACITY];
	char standard_error[OUTPUT_CAPACITY];
};

static int read_output(int descriptor, char *output, size_t output_size)
{
	ssize_t bytes_read;

	if (lseek(descriptor, 0, SEEK_SET) < 0)
		return -1;
	bytes_read = read(descriptor, output, output_size - 1);
	if (bytes_read < 0)
		return -1;
	output[bytes_read] = '\0';
	return 0;
}

static int run_version(const char *option, struct command_result *result)
{
	const char *binary = getenv("LTFS_BINARY");
	char home_template[] = "/tmp/lto-ltfs-home-XXXXXX";
	char stdout_template[] = "/tmp/lto-ltfs-stdout-XXXXXX";
	char stderr_template[] = "/tmp/lto-ltfs-stderr-XXXXXX";
	char *temporary_home;
	int stdout_fd = -1, stderr_fd = -1, status;
	pid_t child;

	if (!binary || !*binary)
		return -1;
	temporary_home = mkdtemp(home_template);
	if (!temporary_home)
		return -1;
	stdout_fd = mkstemp(stdout_template);
	stderr_fd = mkstemp(stderr_template);
	if (stdout_fd < 0 || stderr_fd < 0)
		goto error;
	unlink(stdout_template);
	unlink(stderr_template);

	child = fork();
	if (child < 0)
		goto error;
	if (child == 0) {
		if (setenv("HOME", temporary_home, 1) != 0 ||
			unsetenv("LANG") != 0 ||
			dup2(stdout_fd, STDOUT_FILENO) < 0 ||
			dup2(stderr_fd, STDERR_FILENO) < 0)
			_exit(126);
		execl(binary, binary, option, (char *)NULL);
		_exit(127);
	}

	if (waitpid(child, &status, 0) < 0)
		goto error;
	if (read_output(stdout_fd, result->standard_output,
			OUTPUT_CAPACITY) != 0 ||
		read_output(stderr_fd, result->standard_error,
			OUTPUT_CAPACITY) != 0)
		goto error;
	if (WIFEXITED(status))
		result->exit_code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		result->exit_code = 128 + WTERMSIG(status);
	else
		result->exit_code = -1;

	close(stdout_fd);
	close(stderr_fd);
	rmdir(temporary_home);
	return 0;

error:
	if (stdout_fd >= 0)
		close(stdout_fd);
	if (stderr_fd >= 0)
		close(stderr_fd);
	unlink(stdout_template);
	unlink(stderr_template);
	rmdir(home_template);
	return -1;
}

static int check_version_option(const char *option)
{
	struct command_result result;

	memset(&result, 0, sizeof(result));
	CHECK_INT_EQ(run_version(option, &result), 0);
	CHECK_INT_EQ(result.exit_code, EXIT_SUCCESS);
	CHECK_STR_EQ(result.standard_output, "lto-ltfs 0.1.0\n");
	CHECK_STR_EQ(result.standard_error, "");
	return 0;
}

int main(void)
{
	int failures = 0;

	puts("1..2");
	if (check_version_option("--version") == 0)
		puts("ok 1 - --version needs no configuration or device");
	else {
		puts("not ok 1 - --version needs no configuration or device");
		++failures;
	}
	if (check_version_option("-V") == 0)
		puts("ok 2 - -V needs no configuration or device");
	else {
		puts("not ok 2 - -V needs no configuration or device");
		++failures;
	}

	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
