/* SPDX-License-Identifier: BSD-3-Clause */
/* Exercise the real endian API at every byte alignment without device access. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/libltfs/ltfs_endian.h"
#include "test.h"

#define GUARD 0xa5
#define START 8

union aligned_bytes {
	uint64_t alignment;
	unsigned char bytes[32];
};

static int guards_untouched(const union aligned_bytes *backing,
		const size_t offset, const size_t width)
{
	for (size_t i = 0; i < sizeof(backing->bytes); ++i)
		if ((i < START + offset || i >= START + offset + width) &&
				backing->bytes[i] != GUARD)
			return 0;
	return 1;
}

static int test_u16tobe(void)
{
	static const struct { uint16_t value; unsigned char bytes[2]; } vectors[] = {
		{UINT16_C(0), {0x00, 0x00}},
		{UINT16_MAX, {0xff, 0xff}},
		{UINT16_C(0x12a5), {0x12, 0xa5}},
		{UINT16_C(0x8001), {0x80, 0x01}},
	};
	for (size_t offset = 0; offset < 8; ++offset) for (size_t v = 0; v < 4; ++v) {
		union aligned_bytes backing;
		memset(backing.bytes, GUARD, sizeof(backing.bytes));
		ltfs_u16tobe(backing.bytes + START + offset, vectors[v].value);
		CHECK_TRUE(!memcmp(backing.bytes + START + offset, vectors[v].bytes, 2));
		CHECK_TRUE(guards_untouched(&backing, offset, 2));
	}
	return 0;
}

static int test_u32tobe(void)
{
	static const struct { uint32_t value; unsigned char bytes[4]; } vectors[] = {
		{UINT32_C(0), {0x00, 0x00, 0x00, 0x00}},
		{UINT32_MAX, {0xff, 0xff, 0xff, 0xff}},
		{UINT32_C(0x12a5c37e), {0x12, 0xa5, 0xc3, 0x7e}},
		{UINT32_C(0x80000001), {0x80, 0x00, 0x00, 0x01}},
	};
	for (size_t offset = 0; offset < 8; ++offset) for (size_t v = 0; v < 4; ++v) {
		union aligned_bytes backing;
		memset(backing.bytes, GUARD, sizeof(backing.bytes));
		ltfs_u32tobe(backing.bytes + START + offset, vectors[v].value);
		CHECK_TRUE(!memcmp(backing.bytes + START + offset, vectors[v].bytes, 4));
		CHECK_TRUE(guards_untouched(&backing, offset, 4));
	}
	return 0;
}

static int test_u64tobe(void)
{
	static const struct { uint64_t value; unsigned char bytes[8]; } vectors[] = {
		{UINT64_C(0), {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
		{UINT64_MAX, {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
		{UINT64_C(0x0123456789abcdef), {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}},
		{UINT64_C(0x8000000000000001), {0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}},
	};
	for (size_t offset = 0; offset < 8; ++offset) for (size_t v = 0; v < 4; ++v) {
		union aligned_bytes backing;
		memset(backing.bytes, GUARD, sizeof(backing.bytes));
		ltfs_u64tobe(backing.bytes + START + offset, vectors[v].value);
		CHECK_TRUE(!memcmp(backing.bytes + START + offset, vectors[v].bytes, 8));
		CHECK_TRUE(guards_untouched(&backing, offset, 8));
	}
	return 0;
}

static int test_betou16(void)
{
	static const struct { unsigned char bytes[2]; uint16_t value; } vectors[] = {
		{{0x00,0x00}, UINT16_C(0)}, {{0xff,0xff}, UINT16_MAX},
		{{0x12,0xa5}, UINT16_C(0x12a5)}, {{0x80,0x01}, UINT16_C(0x8001)},
	};
	for (size_t offset = 0; offset < 8; ++offset) for (size_t v = 0; v < 4; ++v) {
		union aligned_bytes input;
		memset(input.bytes, GUARD, sizeof(input.bytes));
		memcpy(input.bytes + START + offset, vectors[v].bytes, 2);
		CHECK_INT_EQ(ltfs_betou16(input.bytes + START + offset), vectors[v].value);
		CHECK_TRUE(!memcmp(input.bytes + START + offset, vectors[v].bytes, 2));
		CHECK_TRUE(guards_untouched(&input, offset, 2));
	}
	return 0;
}

static int test_betou32(void)
{
	static const struct { unsigned char bytes[4]; uint32_t value; } vectors[] = {
		{{0x00,0x00,0x00,0x00}, UINT32_C(0)}, {{0xff,0xff,0xff,0xff}, UINT32_MAX},
		{{0x12,0xa5,0xc3,0x7e}, UINT32_C(0x12a5c37e)},
		{{0x80,0x00,0x00,0x01}, UINT32_C(0x80000001)},
	};
	for (size_t offset = 0; offset < 8; ++offset) for (size_t v = 0; v < 4; ++v) {
		union aligned_bytes input;
		memset(input.bytes, GUARD, sizeof(input.bytes));
		memcpy(input.bytes + START + offset, vectors[v].bytes, 4);
		CHECK_TRUE(ltfs_betou32(input.bytes + START + offset) == vectors[v].value);
		CHECK_TRUE(!memcmp(input.bytes + START + offset, vectors[v].bytes, 4));
		CHECK_TRUE(guards_untouched(&input, offset, 4));
	}
	return 0;
}

static int test_betou64(void)
{
	static const struct { unsigned char bytes[8]; uint64_t value; } vectors[] = {
		{{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, UINT64_C(0)},
		{{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}, UINT64_MAX},
		{{0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef}, UINT64_C(0x0123456789abcdef)},
		{{0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01}, UINT64_C(0x8000000000000001)},
	};
	for (size_t offset = 0; offset < 8; ++offset) for (size_t v = 0; v < 4; ++v) {
		union aligned_bytes input;
		memset(input.bytes, GUARD, sizeof(input.bytes));
		memcpy(input.bytes + START + offset, vectors[v].bytes, 8);
		CHECK_TRUE(ltfs_betou64(input.bytes + START + offset) == vectors[v].value);
		CHECK_TRUE(!memcmp(input.bytes + START + offset, vectors[v].bytes, 8));
		CHECK_TRUE(guards_untouched(&input, offset, 8));
	}
	return 0;
}

struct operation { const char *name; int (*run)(void); };

int main(int argc, char **argv)
{
	static const struct operation operations[] = {
		{"u16tobe", test_u16tobe}, {"u32tobe", test_u32tobe},
		{"u64tobe", test_u64tobe}, {"betou16", test_betou16},
		{"betou32", test_betou32}, {"betou64", test_betou64},
	};
	if (argc == 1) {
		for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
			int ret = operations[i].run();
			if (ret)
				return ret;
		}
		return 0;
	}
	CHECK_INT_EQ(argc, 2);
	for (size_t i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i)
		if (!strcmp(argv[1], operations[i].name))
			return operations[i].run();
	fprintf(stderr, "unknown endian operation: %s\n", argv[1]);
	return 1;
}
