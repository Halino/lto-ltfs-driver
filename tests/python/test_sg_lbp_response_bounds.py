#!/usr/bin/env python3
"""Compile and exercise the real SG LBP/close functions with stub transport."""

from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/tape_drivers/linux/sg/sg_tape.c"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for offset in range(brace, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start : offset + 1]
    raise AssertionError(f"unterminated function: {signature}")


class SgLbpResponseBoundsTests(unittest.TestCase):
    def test_real_lbp_and_close_functions_obey_vendor_and_response_bounds(self):
        source = SOURCE.read_text(encoding="utf-8")
        set_lbp = extract_function(source, "static int _set_lbp(void *device, bool enable)")
        close = extract_function(source, "int sg_close(void *device)")
        harness = PRELUDE + "\n" + set_lbp + "\n" + close + "\n" + TESTS

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            program = root / "test_sg_lbp_response_bounds.c"
            binary = root / "test_sg_lbp_response_bounds"
            program.write_text(harness, encoding="utf-8")
            command = (
                shlex.split(os.environ.get("CC", "cc"))
                + shlex.split(os.environ.get("CFLAGS", ""))
                + ["-std=c11", "-O0", "-Wall", "-Wextra", "-Werror",
                   str(program), "-o", str(binary)]
                + shlex.split(os.environ.get("LDFLAGS", ""))
            )
            compile_result = subprocess.run(
                command,
                check=False, capture_output=True, text=True,
            )
            self.assertEqual(0, compile_result.returncode, compile_result.stderr)
            cases = (
                "exact24", "transport-padding", "ps-bit", "select-failure",
                "no-block-descriptor", "valid48", "lto7-crc32c",
                "bad-type", "bad-subpage", "truncated-header", "overlong-return",
                "bad-page-length", "bad-block-descriptor", "bad-mode-header",
                "failed-control", "enterprise-truncated", "enterprise-failed",
                "enterprise-bad-header", "enterprise-bad-page", "enterprise-valid",
                "hp-close", "ibm-close",
            )
            for case in cases:
                with self.subTest(case=case):
                    result = subprocess.run(
                        [str(binary), case], check=False, capture_output=True, text=True,
                    )
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.assertEqual('', result.stderr, 'unexpected runtime diagnostic: ' + result.stderr)


PRELUDE = r'''
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDEV_UNKNOWN 99
#define EDEV_INTERNAL_ERROR 100
#define DEVICE_GOOD 0
#define LBP_DISABLE 0x00
#define REED_SOLOMON_CRC 0x01
#define CRC32C_CRC 0x02
#define TC_MP_INIT_EXT_LBP_RS 0x40
#define TC_MP_INIT_EXT_LBP_CRC32C 0x20
#define TC_MP_SUB_DP_CTRL_SIZE 48
#define TC_MP_INIT_EXT_SIZE 40
#define TC_MP_INIT_EXT 0x24
#define TC_MP_CTRL 0x0a
#define TC_MP_SUB_DP_CTRL 0xf0
#define TC_MP_PC_CURRENT 0
#define TAPE_FAMILY_ENTERPRISE 0x1000
#define IS_ENTERPRISE(value) ((value) & TAPE_FAMILY_ENTERPRISE)
#define DRIVE_GEN(value) ((value) & 0xff)
#define DRIVE_LTO6 0x2106
#define DRIVE_ENTERPRISE 0x1001
#define VENDOR_IBM 1
#define VENDOR_HP 2
#define LTFS_DEBUG 0
#define LTFS_INFO 0
#define ltfsmsg(...) ((void)0)
#define REQ_TC_CLOSE 1
#define TAPEBEND_REQ_ENTER(value) (value)
#define TAPEBEND_REQ_EXIT(value) (value)

typedef void (*crc_enc)(void);
typedef void (*crc_check)(void);
typedef int TC_MP_PC_TYPE;
struct timeout_tape { int unused; };
struct sg_tape { int fd; };
struct tc_drive_info { int host; int channel; };
struct sg_data {
    struct sg_tape dev;
    int vendor;
    int drive_type;
    crc_enc f_crc_enc;
    crc_check f_crc_check;
    struct timeout_tape *timeouts;
    struct tc_drive_info info;
    FILE *profiler;
    char *devname;
};

static void crc32c_enc(void) {}
static void crc32c_check(void) {}
static void rs_gf256_enc(void) {}
static void rs_gf256_check(void) {}

static struct {
    int mode_sense_calls;
    int mode_select_calls;
    int select_result;
    int response_length;
    int enterprise_length;
    unsigned char response[48];
    unsigned char enterprise[40];
    size_t selected_length;
    unsigned char selected[48];
    int register_calls;
    int close_calls;
    int decrement_calls;
    int timeout_destroy_calls;
} wire;

static void reset_wire(void)
{
    static const unsigned char exact24[24] = {
        0x00,0x16,0x00,0x10,0x00,0x00,0x00,0x08,
        0x5a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x4a,0xf0,0x00,0x04,0x00,0x00,0x00,0x00,
    };
    memset(&wire, 0, sizeof(wire));
    wire.response_length = 24;
    wire.enterprise_length = 40;
    memset(wire.response, 0xa5, sizeof(wire.response));
    memcpy(wire.response, exact24, sizeof(exact24));
}

int sg_modesense(void *device, const unsigned char page, const TC_MP_PC_TYPE pc,
                 const unsigned char subpage, unsigned char *buf, const size_t size)
{
    (void)device; (void)pc;
    ++wire.mode_sense_calls;
    memset(buf, 0xa5, size);
    if (page == TC_MP_INIT_EXT) {
        if (wire.enterprise_length < 0) return wire.enterprise_length;
        size_t copied = (size_t)wire.enterprise_length < size ?
            (size_t)wire.enterprise_length : size;
        memcpy(buf, wire.enterprise, copied);
        return wire.enterprise_length;
    }
    if (page != TC_MP_CTRL || subpage != TC_MP_SUB_DP_CTRL) return -1;
    if (wire.response_length < 0) return wire.response_length;
    size_t copied = (size_t)wire.response_length < size ?
        (size_t)wire.response_length : size;
    memcpy(buf, wire.response, copied);
    return wire.response_length;
}

int sg_modeselect(void *device, unsigned char *buf, const size_t size)
{
    (void)device;
    ++wire.mode_select_calls;
    wire.selected_length = size;
    memcpy(wire.selected, buf, size);
    return wire.select_result;
}

static int _register_key(void *device, unsigned char *key)
{ (void)device; (void)key; ++wire.register_calls; return 0; }
void ltfs_profiler_add_entry(FILE *profiler, const char *label, int request)
{ (void)profiler; (void)label; (void)request; }
int close(int fd) { (void)fd; ++wire.close_calls; return 0; }
void decrement_openfactor(int host, int channel)
{ (void)host; (void)channel; ++wire.decrement_calls; }
void ibm_tape_destroy_timeout(struct timeout_tape **timeouts)
{ (void)timeouts; ++wire.timeout_destroy_calls; }

static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "%s\n", message); exit(1); }
}
'''

TESTS = r'''
static struct sg_data lto(int generation)
{
    struct sg_data device = {.vendor = VENDOR_IBM, .drive_type = 0x2100 | generation};
    return device;
}

static void expect_refused(struct sg_data *device, const char *message)
{
    require(_set_lbp(device, false) < 0, message);
    require(wire.mode_select_calls == 0, "invalid response reached MODE SELECT");
}

static void exact24(void)
{
    struct sg_data device = lto(6);
    reset_wire();
    require(_set_lbp(&device, false) == DEVICE_GOOD, "24-byte response rejected");
    require(wire.mode_select_calls == 1, "24-byte response did not select");
    require(wire.selected_length == 24, "MODE SELECT sent beyond actual response");
    for (size_t i = 24; i < sizeof(wire.selected); ++i)
        require(wire.selected[i] == 0, "poisoned stack tail was sent");
}

static void transport_padding(void)
{
    struct sg_data device = lto(6);
    reset_wire(); wire.response_length = 48;
    require(_set_lbp(&device, false) == DEVICE_GOOD, "padded response rejected");
    require(wire.mode_select_calls == 1, "padded response did not select");
    require(wire.selected_length == 24, "transport padding reached MODE SELECT");
    for (size_t i = 24; i < sizeof(wire.selected); ++i)
        require(wire.selected[i] == 0, "padded tail was sent");
}

static void ps_bit(void)
{
    struct sg_data device = lto(6);
    reset_wire();
    wire.response[16] |= 0x80;
    wire.response[15] = 0x5c;
    wire.response[23] = 0x6d;
    require(_set_lbp(&device, false) == DEVICE_GOOD, "PS-bit response rejected");
    require(wire.selected[16] == 0x4a, "PS bit was not cleared for MODE SELECT");
    require(wire.selected[15] == 0x5c && wire.selected[23] == 0x6d,
            "unrelated response bytes changed");
}

static void select_failure(void)
{
    struct sg_data device = lto(6);
    device.f_crc_enc = crc32c_enc;
    device.f_crc_check = crc32c_check;
    reset_wire(); wire.select_result = -11;
    require(_set_lbp(&device, false) == -11, "MODE SELECT failure not propagated");
    require(device.f_crc_enc == crc32c_enc && device.f_crc_check == crc32c_check,
            "failed MODE SELECT changed CRC callbacks");

    struct sg_data *ibm = calloc(1, sizeof(*ibm));
    require(ibm != NULL, "allocation failed");
    ibm->vendor = VENDOR_IBM; ibm->drive_type = DRIVE_LTO6; ibm->dev.fd = 11;
    ibm->f_crc_enc = crc32c_enc; ibm->f_crc_check = crc32c_check;
    reset_wire(); wire.select_result = -11;
    require(sg_close(ibm) == 0, "IBM close exposed best-effort LBP failure");
    require(wire.mode_select_calls == 1 && wire.register_calls == 1 &&
            wire.close_calls == 1 && wire.decrement_calls == 1 &&
            wire.timeout_destroy_calls == 1,
            "IBM close failure skipped ordinary cleanup");
}

static void no_block_descriptor(void)
{
    struct sg_data device = lto(6);
    reset_wire();
    memset(wire.response, 0, sizeof(wire.response));
    memset(wire.response + 24, 0xa5, sizeof(wire.response) - 24);
    wire.response[1] = 22;
    wire.response[3] = 0x10;
    wire.response[7] = 0;
    wire.response[8] = 0x4a;
    wire.response[9] = 0xf0;
    wire.response[10] = 0;
    wire.response[11] = 12;
    require(_set_lbp(&device, true) == DEVICE_GOOD, "no-BD response rejected");
    require(wire.selected_length == 24, "no-BD response length changed");
    require(wire.selected[12] == REED_SOLOMON_CRC && wire.selected[13] == 4 &&
            wire.selected[14] == 0xc0, "no-BD page offsets ignored");
}

static void valid48(void)
{
    struct sg_data device = lto(6);
    reset_wire(); wire.response_length = 48; wire.response[1] = 46; wire.response[19] = 28;
    require(_set_lbp(&device, true) == DEVICE_GOOD, "48-byte response rejected");
    require(wire.selected_length == 48, "48-byte response length changed");
    require(wire.selected[20] == REED_SOLOMON_CRC, "RS method not selected");
    require(wire.selected[21] == 0x04 && wire.selected[22] == 0xc0,
            "LBP enable fields changed");
}

static void lto7_crc32c(void)
{
    struct sg_data device = lto(7);
    reset_wire();
    require(_set_lbp(&device, true) == DEVICE_GOOD, "LTO7 response rejected");
    require(wire.selected[20] == CRC32C_CRC, "LTO7 did not select CRC32C");
}

static void bad_type(void) { struct sg_data d = lto(6); reset_wire(); wire.response[16] = 0x0a; expect_refused(&d, "non-subpage type accepted"); }
static void bad_subpage(void) { struct sg_data d = lto(6); reset_wire(); wire.response[17] = 0xef; expect_refused(&d, "wrong subpage accepted"); }
static void truncated_header(void) { struct sg_data d = lto(6); reset_wire(); wire.response_length = 7; expect_refused(&d, "truncated mode header accepted"); }
static void overlong_return(void) { struct sg_data d = lto(6); reset_wire(); wire.response_length = 49; expect_refused(&d, "overlong transport return accepted"); }
static void bad_page_length(void) { struct sg_data d = lto(6); reset_wire(); wire.response[19] = 5; expect_refused(&d, "over-declared page length accepted"); }
static void bad_block_descriptor(void) { struct sg_data d = lto(6); reset_wire(); wire.response[7] = 9; expect_refused(&d, "misaligned block descriptor accepted"); }
static void bad_mode_header(void) { struct sg_data d = lto(6); reset_wire(); wire.response[1] = 23; expect_refused(&d, "over-declared mode length accepted"); }
static void failed_control(void) { struct sg_data d = lto(6); reset_wire(); wire.response_length = -7; require(_set_lbp(&d, false) == -7, "MODE SENSE failure not preserved"); require(wire.mode_select_calls == 0, "failed response reached MODE SELECT"); }

static void valid_enterprise_fixture(void)
{
    memset(wire.enterprise, 0, sizeof(wire.enterprise));
    wire.enterprise[1] = 38;
    wire.enterprise[3] = 0x10;
    wire.enterprise[7] = 8;
    wire.enterprise[16] = 0x24;
    wire.enterprise[17] = 22;
    wire.enterprise[18] = TC_MP_INIT_EXT_LBP_CRC32C;
}

static void enterprise_truncated(void) { struct sg_data d = {.vendor=VENDOR_IBM,.drive_type=DRIVE_ENTERPRISE}; reset_wire(); wire.enterprise_length=18; require(_set_lbp(&d,true)<0,"truncated enterprise page accepted"); require(wire.mode_select_calls==0,"truncated extension selected"); }
static void enterprise_failed(void) { struct sg_data d = {.vendor=VENDOR_IBM,.drive_type=DRIVE_ENTERPRISE}; reset_wire(); wire.enterprise_length=-9; require(_set_lbp(&d,true)==-9,"enterprise failure not preserved"); require(wire.mode_select_calls==0,"failed extension selected"); }
static void enterprise_bad_header(void) { struct sg_data d = {.vendor=VENDOR_IBM,.drive_type=DRIVE_ENTERPRISE}; reset_wire(); valid_enterprise_fixture(); wire.enterprise[1]=39; require(_set_lbp(&d,true)<0,"bad enterprise header accepted"); require(wire.mode_select_calls==0,"bad extension selected"); }
static void enterprise_bad_page(void) { struct sg_data d = {.vendor=VENDOR_IBM,.drive_type=DRIVE_ENTERPRISE}; reset_wire(); valid_enterprise_fixture(); wire.enterprise[16]=0x25; require(_set_lbp(&d,true)<0,"wrong enterprise page accepted"); require(wire.mode_select_calls==0,"wrong extension selected"); }
static void enterprise_valid(void) { struct sg_data d = {.vendor=VENDOR_IBM,.drive_type=DRIVE_ENTERPRISE}; reset_wire(); valid_enterprise_fixture(); require(_set_lbp(&d,true)==DEVICE_GOOD,"valid enterprise page rejected"); require(wire.selected[20]==CRC32C_CRC,"enterprise CRC32C ignored"); }

static void hp_close(void)
{
    struct sg_data *hp = calloc(1, sizeof(*hp));
    require(hp != NULL, "allocation failed");
    hp->vendor = VENDOR_HP; hp->drive_type = DRIVE_LTO6; hp->dev.fd = 9;
    reset_wire();
    require(sg_close(hp) == 0, "HP close failed");
    require(wire.mode_sense_calls == 0, "HP close attempted IBM LBP teardown");
    require(wire.register_calls == 1 && wire.close_calls == 1 &&
            wire.decrement_calls == 1 && wire.timeout_destroy_calls == 1,
            "HP close skipped ordinary cleanup");
}

static void ibm_close(void)
{
    struct sg_data *ibm = calloc(1, sizeof(*ibm));
    require(ibm != NULL, "allocation failed");
    ibm->vendor = VENDOR_IBM; ibm->drive_type = DRIVE_LTO6; ibm->dev.fd = 10;
    reset_wire();
    require(sg_close(ibm) == 0, "IBM close failed");
    require(wire.mode_sense_calls == 1 && wire.mode_select_calls == 1,
            "IBM close no longer disables LBP");
    require(wire.register_calls == 1 && wire.close_calls == 1 &&
            wire.decrement_calls == 1 && wire.timeout_destroy_calls == 1,
            "IBM close skipped ordinary cleanup");
}

struct test_case { const char *name; void (*run)(void); };
int main(int argc, char **argv)
{
    const struct test_case cases[] = {
        {"exact24", exact24}, {"transport-padding", transport_padding},
        {"ps-bit", ps_bit}, {"select-failure", select_failure},
        {"no-block-descriptor", no_block_descriptor},
        {"valid48", valid48}, {"lto7-crc32c", lto7_crc32c},
        {"bad-type", bad_type}, {"bad-subpage", bad_subpage},
        {"truncated-header", truncated_header}, {"overlong-return", overlong_return},
        {"bad-page-length", bad_page_length}, {"bad-block-descriptor", bad_block_descriptor},
        {"bad-mode-header", bad_mode_header}, {"failed-control", failed_control},
        {"enterprise-truncated", enterprise_truncated}, {"enterprise-failed", enterprise_failed},
        {"enterprise-bad-header", enterprise_bad_header}, {"enterprise-bad-page", enterprise_bad_page},
        {"enterprise-valid", enterprise_valid}, {"hp-close", hp_close}, {"ibm-close", ibm_close},
    };
    require(argc == 2, "one test case argument required");
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
        if (!strcmp(argv[1], cases[i].name)) { cases[i].run(); return 0; }
    fprintf(stderr, "unknown test case: %s\n", argv[1]);
    return 1;
}
'''


if __name__ == "__main__":
    unittest.main()
