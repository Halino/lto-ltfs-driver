/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef LTO_LTFS_TEST_H
#define LTO_LTFS_TEST_H

#include <stdio.h>
#include <string.h>

#define CHECK_TRUE(condition)                                                \
	do {                                                                  \
		if (!(condition)) {                                             \
			fprintf(stderr, "%s:%d: check failed: %s\n",            \
				__FILE__, __LINE__, #condition);                    \
			return 1;                                                 \
		}                                                                 \
	} while (0)

#define CHECK_INT_EQ(actual, expected)                                      \
	do {                                                                  \
		long test_actual = (long)(actual);                              \
		long test_expected = (long)(expected);                          \
		if (test_actual != test_expected) {                             \
			fprintf(stderr, "%s:%d: expected %ld, got %ld\n",       \
				__FILE__, __LINE__, test_expected, test_actual);     \
			return 1;                                                 \
		}                                                                 \
	} while (0)

#define CHECK_STR_EQ(actual, expected)                                      \
	do {                                                                  \
		const char *test_actual = (actual);                              \
		const char *test_expected = (expected);                          \
		if (strcmp(test_actual, test_expected) != 0) {                   \
			fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n", \
				__FILE__, __LINE__, test_expected, test_actual);     \
			return 1;                                                 \
		}                                                                 \
	} while (0)

#endif /* LTO_LTFS_TEST_H */
