/* SPDX-License-Identifier: BSD-3-Clause */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_value(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static char *decode_hex(const char *encoded)
{
	size_t length = strlen(encoded);
	char *decoded;
	size_t index;

	if (length % 2)
		return NULL;
	decoded = malloc(length / 2 + 1);
	if (!decoded)
		return NULL;
	for (index = 0; index < length / 2; ++index) {
		int high = hex_value(encoded[index * 2]);
		int low = hex_value(encoded[index * 2 + 1]);
		if (high < 0 || low < 0 || (!high && !low)) {
			free(decoded);
			return NULL;
		}
		decoded[index] = (char)((high << 4) | low);
	}
	decoded[length / 2] = '\0';
	return decoded;
}

static void print_hex(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;

	putchar(':');
	while (*cursor) {
		printf("%02x", *cursor);
		++cursor;
	}
}

static void print_error(void)
{
	puts("ERR");
}

static void parse_line(char *line)
{
	char *saveptr = NULL;
	char *directive;
	char *first;
	char *second;
	char *third;
	char *strip_pos = strstr(line, "#");

	if (!strip_pos)
		strip_pos = line + strlen(line);
	while (strip_pos > line &&
			(*(strip_pos - 1) == ' ' || *(strip_pos - 1) == '\t' ||
			 *(strip_pos - 1) == '\r' || *(strip_pos - 1) == '\n'))
		--strip_pos;
	*strip_pos = '\0';

	directive = strtok_r(line, " \t\r\n", &saveptr);
	if (!directive) {
		puts("IGNORE");
		return;
	}
	if (!strcmp(directive, "plugin")) {
		first = strtok_r(NULL, " \t\r\n", &saveptr);
		second = strtok_r(NULL, " \t\r\n", &saveptr);
		third = strtok_r(NULL, "\r\n", &saveptr);
		if (!first || !second || !third) {
			print_error();
			return;
		}
		fputs("OK", stdout);
		print_hex(directive);
		print_hex(first);
		print_hex(second);
		print_hex(third);
		putchar('\n');
		return;
	}
	if (!strcmp(directive, "default")) {
		first = strtok_r(NULL, " \t\r\n", &saveptr);
		second = strtok_r(NULL, " \t\r\n", &saveptr);
		third = strtok_r(NULL, " \t\r\n", &saveptr);
		if (!first || !second || third) {
			print_error();
			return;
		}
		fputs("OK", stdout);
		print_hex(directive);
		print_hex(first);
		print_hex(second);
		putchar('\n');
		return;
	}
	if (!strcmp(directive, "include") ||
			!strcmp(directive, "include_noerror")) {
		first = strtok_r(NULL, " \t\r\n", &saveptr);
		second = strtok_r(NULL, " \t\r\n", &saveptr);
		if (!first || second) {
			print_error();
			return;
		}
		fputs("OK", stdout);
		print_hex(directive);
		print_hex(first);
		putchar('\n');
		return;
	}
	puts("IGNORE");
}

int main(int argc, char **argv)
{
	int index;

	for (index = 1; index < argc; ++index) {
		char *line = decode_hex(argv[index]);
		if (!line)
			return 2;
		parse_line(line);
		free(line);
	}
	return 0;
}
