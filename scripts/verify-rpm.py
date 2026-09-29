#!/usr/bin/env python3
"""Verify an lto-ltfs RPM without installing it or accessing hardware."""

import hashlib
import ipaddress
import json
import os
import re
import select
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
from pathlib import Path, PurePosixPath

EXPECTED_EXECUTABLES = {
    "/usr/bin/ltfs",
    "/usr/bin/ltfs-info",
    "/usr/bin/ltfsck",
    "/usr/bin/mkltfs",
}
EXPECTED_SYMLINKS = {
    "/usr/lib64/libltfs.so": "libltfs.so.0.0.0",
    "/usr/lib64/libltfs.so.0": "libltfs.so.0.0.0",
}
CORE_LIBRARY_PATH = "/usr/lib64/libltfs.so.0.0.0"
CONFIG_HEADER_PATH = "/usr/include/ltfs/config.h"
TRACE_HEADER_PATH = "/usr/include/ltfs/libltfs/ltfstrace.h"
SG_PLUGIN_PATH = "/usr/lib64/ltfs/libtape-sg.so"
EXPECTED_PLUGIN_BINARIES = frozenset(
    {
        "/usr/lib64/ltfs/libiosched-fcfs.so",
        "/usr/lib64/ltfs/libiosched-unified.so",
        "/usr/lib64/ltfs/libkmi-flatfile.so",
        "/usr/lib64/ltfs/libkmi-simple.so",
        "/usr/lib64/ltfs/libtape-file.so",
        "/usr/lib64/ltfs/libtape-itdtimg.so",
        SG_PLUGIN_PATH,
    }
)
EXPECTED_BINARY_PATHS = frozenset(
    EXPECTED_EXECUTABLES | {CORE_LIBRARY_PATH} | EXPECTED_PLUGIN_BINARIES
)
HPE_HEADER_LINES_BY_PATH = {
    "/usr/include/ltfs/libltfs/ltfs.h": (
        "\tNOLOCK_MAM    = 128,  /* From HPE */",
    ),
    "/usr/include/ltfs/libltfs/tape_ops.h": (
        "#define TC_MAM_BARCODE_LEN TC_MAM_BARCODE_SIZE /* HPE LTFS alias */",
        "\t * @param vol_name Volume name, unused by libtlfs (HPE extension)",
        "\t * @param vol_name Volume barcode, unused by libtlfs (HPE extension)",
        "\t * @param vol_mam_uuid Volume UUID, unused by libtlfs (HPE extension)",
    ),
    "/usr/include/ltfs/tape_drivers/ibm_tape.h": (
        (
            "\tVOLSTATS_USED_CAPACITY    = 0x0203,\t"
            "/* HPE alias of VOLSTATS_PART_USED_CAP */"
        ),
    ),
}
HPE_SG_FIELDS = (
    "HPE",
    "HPE",
    "HPE",
    "HPE LTO - Cleaning Required",
    "HPE LTO - Bad microcode detected",
)
HPE_SG_OPTIMIZED_FIELDS = (
    "HPE",
    "HPE",
    "HPE LTO - Cleaning Required",
    "HPE LTO - Bad microcode detected",
)
EXPECTED_RELEASE = "22.el9"
EXPECTED_LICENSE = "BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only"
LGPL_TEXT_PATH = "/usr/share/licenses/lto-ltfs/COPYING.LIB"
LGPL_NOTICE_PATH = "/usr/share/licenses/lto-ltfs/LGPL-NOTICE"
LICENSE_INVENTORY_PATH = "/usr/share/doc/lto-ltfs/license-inventory.json"
LICENSE_INVENTORY_SHA256 = "aa779d68fa992c225f589b17846738ca2d208878d9bc25955f64c0d9c7de2e50"
LGPL_TEXT_SHA256 = "e5f56b8b8a25d180fa873f1ad24fac4c0f310c538ae22ec16bb8641ef7902993"
LGPL_NOTICE_SHA256 = "74ebdf36bef70ae39abb6e7eba31eaa45132fde38faeac8a6e07cbe1df3fc114"
EXACT_CONFIG_PATHS = {
    "/etc/ltfs.conf",
    "/etc/ltfs.conf.local",
    "/etc/lto-ltfs/device.json",
}
ALLOWED_DIRECTORIES = {
    "/etc/lto-ltfs",
    "/usr/include/ltfs",
    "/usr/lib64/ltfs",
    "/usr/share/lto-ltfs/messages",
}
REQUIRED_DEPENDENCIES = {
    "/bin/sh",
    "/usr/bin/fusermount",
    "/usr/bin/systemd-tmpfiles",
    "/usr/lib/udev/scsi_id",
    "fuse-libs",
    "libicu",
    "libuuid",
    "libxml2",
    "openssl-libs",
    "shadow-utils",
    "zlib",
}
REQUIRED_DEPENDENCY_LINES = {
    "/bin/sh",
    "/usr/bin/fusermount",
    "/usr/bin/systemd-tmpfiles",
    "/usr/lib/udev/scsi_id",
    "fuse-libs >= 2.9.9",
    "libicu",
    "libuuid",
    "libxml2",
    "openssl-libs",
    "shadow-utils",
    "zlib",
}
ALLOWED_DEPENDENCY_CAPABILITIES = REQUIRED_DEPENDENCIES | {
    "/usr/sbin/groupadd",
    "libc.so.6()(64bit)",
    "libc.so.6(GLIBC_2.14)(64bit)",
    "libc.so.6(GLIBC_2.17)(64bit)",
    "libc.so.6(GLIBC_2.2.5)(64bit)",
    "libc.so.6(GLIBC_2.3.4)(64bit)",
    "libc.so.6(GLIBC_2.34)(64bit)",
    "libc.so.6(GLIBC_2.4)(64bit)",
    "libcrypto.so.3()(64bit)",
    "libcrypto.so.3(OPENSSL_3.0.0)(64bit)",
    "libdl.so.2()(64bit)",
    "libfuse.so.2()(64bit)",
    "libicudata.so.67()(64bit)",
    "libicui18n.so.67()(64bit)",
    "libicuuc.so.67()(64bit)",
    "libm.so.6()(64bit)",
    "libpthread.so.0()(64bit)",
    "librt.so.1()(64bit)",
    "libssl.so.3()(64bit)",
    "libuuid.so.1()(64bit)",
    "libxml2.so.2()(64bit)",
    "libz.so.1()(64bit)",
    "libltfs.so.0()(64bit)",
    "rtld(GNU_HASH)",
}
ALLOWED_AUTO_DEPENDENCY = re.compile(
    r"(?:"
    r"(?:ld-linux-x86-64|lib(?:c|dl|m|pthread|rt))\.so\.[0-9]+"
    r"\(GLIBC_(?:[0-9.]+|ABI_DT_RELR)\)\(64bit\)"
    r"|libgcc_s\.so\.[0-9]+\(GCC_[0-9.]+\)\(64bit\)"
    r"|libstdc\+\+\.so\.[0-9]+"
    r"\((?:GLIBCXX|CXXABI)_[0-9.]+\)\(64bit\)"
    r"|libfuse\.so\.2\(FUSE_[0-9.]+\)\(64bit\)"
    r"|libuuid\.so\.1\(UUID_[0-9.]+\)\(64bit\)"
    r"|libxml2\.so\.2\(LIBXML2_[0-9.]+\)\(64bit\)"
    r"|lib(?:ssl|crypto)\.so\.3\(OPENSSL_[0-9.]+\)\(64bit\)"
    r")"
)
ALLOWED_EXACT_AUTO_DEPENDENCIES = {
    "/usr/sbin/groupadd",
    "/usr/bin/pkg-config",  # Auto-require for the installed ltfs.pc metadata.
    "config(lto-ltfs) = 0.1.0-22.el9",
    "rpmlib(CompressedFileNames) <= 3.0.4-1",
    "rpmlib(FileDigests) <= 4.6.0-1",
    "rpmlib(PayloadFilesHavePrefix) <= 4.0-1",
    "rpmlib(PayloadIsZstd) <= 5.4.18-1",
}
REQUIRED_PATHS = {
    "/etc/lto-ltfs/device.json",
    CONFIG_HEADER_PATH,
    SG_PLUGIN_PATH,
    "/usr/lib/tmpfiles.d/lto-ltfs.conf",
    "/usr/lib/udev/rules.d/99-lto-ltfs.rules",
} | set(HPE_HEADER_LINES_BY_PATH)
DOCUMENTATION_NETWORKS = tuple(
    ipaddress.ip_network(value)
    for value in ("192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24")
)
CANONICAL_PRODUCT_VERSION = "2.4.8.4 (Prelim)"
CANONICAL_UPSTREAM_TAG = "v2.4.8.4-10522"
CANONICAL_CONFIG_HEADER_LINES = frozenset(
    {
        '#define PACKAGE_STRING "LTFS ' + CANONICAL_PRODUCT_VERSION + '"',
        '#define PACKAGE_VERSION "' + CANONICAL_PRODUCT_VERSION + '"',
        '#define VERSION "' + CANONICAL_PRODUCT_VERSION + '"',
    }
)
IPV4 = re.compile(r"(?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.])")
SECRET = re.compile(
    r"(?i)(?:BEGIN [A-Z ]*PRIVATE KEY|(?:password|passwd|secret|token|api[_-]?key)\s*[\":=])"
)
PRIVATE_PATH = re.compile(r"(?:/root/|/home/[^/\s]+/|/Users/[^/\s]+/)")
FORBIDDEN_PRODUCT = re.compile(
    r"(?i)(?:storeopen|linear tape file system diagnostics|\bjre\b)"
)
HPE_MARKER = re.compile(r"(?i)hpe")
TAPE_SCRIPT = re.compile(
    r"(?im)(?:/dev/(?!null\b)|(?:^|[;&|]\s*|\n)\s*"
    r"(?:/usr/(?:s?bin)/)?(?:mkltfs|ltfsck|ltfs|mt|eject|sg_[a-z0-9_-]+)\b)"
)
DEVICE_SCRIPT = re.compile(
    r"(?im)(?:^|[;&|]\s*|\n)\s*(?:/usr/(?:s?bin)/)?(?:udevadm|mknod)\b"
)
OLD_ICU = re.compile(r"libicu(?:uc|i18n|data)?\.so\.([0-9]+)")
DEVICE_SERIAL = re.compile(r"(?<![A-Z0-9])[A-Z]{1,3}[0-9]{7,12}(?![A-Z0-9])")
TRACE_STATUS_MASK_LINE = "#define REQ_STATUS_MASK (0xF0000000)"
EXPECTED_SCRIPTS = (
    "preinstall scriptlet (using /bin/sh):\n"
    "getent group lto-admin >/dev/null || /usr/sbin/groupadd -r lto-admin\n"
    "postinstall scriptlet (using /bin/sh):\n"
    "/usr/bin/systemd-tmpfiles --create /usr/lib/tmpfiles.d/lto-ltfs.conf\n"
)
RPM_SENSE_SCRIPT_POST = 1 << 10
SYSTEMD_TMPFILES_POST_REQUIRE = (
    "/usr/bin/systemd-tmpfiles",
    RPM_SENSE_SCRIPT_POST,
    "",
)
MAX_PAYLOAD_FILE_SIZE = 64 * 1024 * 1024
MAX_PAYLOAD_SCAN_SIZE = 512 * 1024 * 1024
MAX_PAYLOAD_ENTRIES = 1024
MAX_PAYLOAD_PATH_SIZE = 4096
MAX_QUERY_OUTPUT_SIZE = 16 * 1024 * 1024
MAX_SOURCE_MANIFEST_SIZE = 4096
MAX_CPIO_STREAM_SIZE = MAX_PAYLOAD_SCAN_SIZE + MAX_PAYLOAD_ENTRIES * (
    110 + MAX_PAYLOAD_PATH_SIZE + 8
)


class VerificationError(ValueError):
    pass


def fail(message):
    raise VerificationError(message)


def terminate_process(process):
    if process.poll() is None:
        process.kill()
    process.wait()


def read_process_output(process, output_limit, timeout):
    assert process.stdout is not None
    descriptor = process.stdout.fileno()
    deadline = time.monotonic() + timeout
    output = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            terminate_process(process)
            fail("RPM query tool is unavailable or timed out")
        readable, _, _ = select.select([descriptor], [], [], remaining)
        if not readable:
            terminate_process(process)
            fail("RPM query tool is unavailable or timed out")
        block = os.read(descriptor, min(64 * 1024, output_limit + 1 - len(output)))
        if not block:
            break
        output.extend(block)
        if len(output) > output_limit:
            terminate_process(process)
            fail("RPM query output exceeds the verification limit")
    try:
        process.wait(timeout=max(0.01, deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        terminate_process(process)
        fail("RPM query tool is unavailable or timed out")
    return bytes(output)


def run_query(arguments):
    try:
        process = subprocess.Popen(
            arguments,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        fail("RPM query tool is unavailable or timed out")
    output = read_process_output(process, MAX_QUERY_OUTPUT_SIZE, 30)
    if process.returncode != 0:
        fail("RPM query failed")
    try:
        return output.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        fail("RPM query returned non-UTF-8 metadata")


def read_metadata(rpm_path, source=False):
    query = (
        "%{NAME}\t%{VERSION}\t%{RELEASE}\t%{ARCH}\t%{LICENSE}\t"
        "%{PAYLOADDIGEST}\t%|SOURCEPACKAGE?{%{SOURCEPACKAGE}}:{0}|\\n"
    )
    fields = run_query(["rpm", "-qp", "--qf", query, str(rpm_path)]).strip().split("\t")
    if len(fields) != 7:
        fail("RPM metadata schema is invalid")
    (
        name,
        version,
        release,
        architecture,
        license_name,
        payload_digest,
        source_package,
    ) = fields
    if name != "lto-ltfs" or version != "0.1.0":
        fail("RPM name or version is unexpected")
    if release != EXPECTED_RELEASE:
        fail("RPM release is unexpected")
    if source_package != ("1" if source else "0"):
        fail("RPM source/binary identity is unexpected")
    if not source and architecture != "x86_64":
        fail("RPM binary architecture is unexpected")
    if license_name != EXPECTED_LICENSE:
        fail("RPM license is unexpected")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", payload_digest):
        fail("RPM payload digest is invalid")


def safe_payload_path(path):
    candidate = PurePosixPath(path)
    return (
        candidate.is_absolute()
        and ".." not in candidate.parts
        and not any(ord(character) < 32 for character in path)
    )


def path_is_allowed(path):
    if (
        path in EXPECTED_BINARY_PATHS
        or path in EXPECTED_SYMLINKS
        or path in EXACT_CONFIG_PATHS
    ):
        return True
    if path in ALLOWED_DIRECTORIES:
        return True
    if path.startswith("/usr/include/ltfs/"):
        return True
    if path == "/usr/lib64/pkgconfig/ltfs.pc":
        return True
    if path in {
        "/usr/lib/udev/rules.d/99-lto-ltfs.rules",
        "/usr/lib/tmpfiles.d/lto-ltfs.conf",
    }:
        return True
    if path.startswith("/usr/share/lto-ltfs/messages/"):
        return True
    if re.fullmatch(r"/usr/share/(?:doc|licenses)/lto-ltfs(?:-0\.1\.0)?(?:/.*)?", path):
        return True
    return bool(
        re.fullmatch(
            r"/usr/share/man/man(?:1|8)/(?:ltfs-sde\.8|ltfs_ordered_copy\.1|ltfsck\.8|mkltfs\.8)(?:\.gz)?",
            path,
        )
    )


def read_file_records(rpm_path):
    query = (
        "[%{FILEMODES:octal}\\t%{FILEUSERNAME}\\t%{FILEGROUPNAME}\\t%{FILENAMES}\\n]"
    )
    output = run_query(["rpm", "-qp", "--qf", query, str(rpm_path)])
    records = {}
    for count, line in enumerate(output.splitlines(), start=1):
        if count > MAX_PAYLOAD_ENTRIES:
            fail("RPM payload has too many entries")
        fields = line.split("\t")
        if len(fields) != 4:
            fail("RPM file metadata schema is invalid")
        encoded_mode, owner, group, path = fields
        if len(path.encode("utf-8")) > MAX_PAYLOAD_PATH_SIZE:
            fail("RPM payload path exceeds the verification limit")
        try:
            mode = int(encoded_mode, 8)
        except ValueError:
            fail("RPM file mode is invalid")
        if not safe_payload_path(path) or path in records:
            fail("forbidden path or duplicate payload path")
        if path.startswith("/usr/bin/") and path not in EXPECTED_EXECUTABLES:
            fail("unexpected executable in RPM payload")
        if not path_is_allowed(path):
            fail("forbidden path in RPM payload")
        basename = PurePosixPath(path).name.lower()
        if HPE_MARKER.search(path) or FORBIDDEN_PRODUCT.search(path) or any(
            token in basename for token in ("hpe", "storeopen", "ltt", "jre", "java")
        ):
            fail("forbidden vendor or JRE artifact in RPM payload")
        file_type = stat.S_IFMT(mode)
        if file_type not in (stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK):
            fail("special file in RPM payload")
        if mode & (stat.S_ISUID | stat.S_ISGID):
            fail("set-id file in RPM payload")
        if file_type in (stat.S_IFREG, stat.S_IFDIR) and mode & stat.S_IWOTH:
            fail("world-writable file in RPM payload")
        records[path] = (mode, owner, group)
    if not records:
        fail("RPM payload is empty")
    return records


def verify_files(records):
    paths = set(records)
    for path in (
        "/usr/share/licenses/lto-ltfs/LICENSE",
        "/usr/share/licenses/lto-ltfs/NOTICES",
        "/usr/share/licenses/lto-ltfs/upstream.json",
        LGPL_TEXT_PATH,
        LGPL_NOTICE_PATH,
    ):
        record = records.get(path)
        if record is None or record != (stat.S_IFREG | 0o644, "root", "root"):
            fail("required RPM license payload is missing or unsafe")
    if not EXPECTED_EXECUTABLES.issubset(paths):
        fail("expected executable is missing")
    if not REQUIRED_PATHS.issubset(paths):
        fail("required configuration payload is missing")
    header_mode, header_owner, header_group = records[CONFIG_HEADER_PATH]
    if (
        not stat.S_ISREG(header_mode)
        or header_owner != "root"
        or header_group != "root"
        or header_mode & stat.S_IWOTH
    ):
        fail("installed LTFS configuration header metadata is invalid")
    core_record = records.get(CORE_LIBRARY_PATH)
    if core_record is None:
        fail("LTFS core library is missing")
    core_mode, core_owner, core_group = core_record
    if (
        not stat.S_ISREG(core_mode)
        or core_owner != "root"
        or core_group != "root"
        or core_mode & stat.S_IWOTH
    ):
        fail("LTFS core library metadata is invalid")
    symlinks = {
        path for path, (mode, _owner, _group) in records.items() if stat.S_ISLNK(mode)
    }
    expected_symlink_record = (stat.S_IFLNK | 0o777, "root", "root")
    if symlinks != set(EXPECTED_SYMLINKS) or any(
        records[path] != expected_symlink_record for path in EXPECTED_SYMLINKS
    ):
        fail("RPM payload symlink metadata is outside the exact allowlist")
    required_prefixes = (
        "/usr/include/ltfs/",
        "/usr/lib64/ltfs/",
        "/usr/share/licenses/lto-ltfs/",
        "/usr/share/lto-ltfs/messages/",
        "/usr/share/man/",
    )
    for prefix in required_prefixes:
        if not any(path.startswith(prefix) for path in paths):
            fail("required RPM payload class is missing")
    for executable in EXPECTED_EXECUTABLES - {"/usr/bin/mkltfs"}:
        mode, owner, group = records[executable]
        if stat.S_IMODE(mode) != 0o755 or owner != "root" or group != "root":
            fail("runtime executable permissions are invalid")
    mkltfs_mode, mkltfs_owner, mkltfs_group = records["/usr/bin/mkltfs"]
    if (
        stat.S_IMODE(mkltfs_mode) != 0o750
        or mkltfs_owner != "root"
        or mkltfs_group != "lto-admin"
    ):
        fail("mkltfs must be mode 0750 and owned by root:lto-admin")
    protected_files = {
        "/etc/lto-ltfs/device.json": (
            0o640,
            "root",
            "lto-admin",
            "device selector",
        ),
        "/etc/ltfs.conf": (0o644, "root", "root", "LTFS configuration"),
        "/etc/ltfs.conf.local": (
            0o640,
            "root",
            "lto-admin",
            "LTFS local configuration",
        ),
        "/usr/lib/tmpfiles.d/lto-ltfs.conf": (0o644, "root", "root", "tmpfiles policy"),
        "/usr/lib/udev/rules.d/99-lto-ltfs.rules": (
            0o644,
            "root",
            "root",
            "udev policy",
        ),
    }
    for path, (
        expected_mode,
        expected_owner,
        expected_group,
        label,
    ) in protected_files.items():
        mode, owner, group = records[path]
        if (
            stat.S_IMODE(mode) != expected_mode
            or owner != expected_owner
            or group != expected_group
        ):
            fail(label + " permissions are invalid")


def verify_dependencies(rpm_path):
    dependency_lines = {
        line.strip()
        for line in run_query(["rpm", "-qp", "--requires", str(rpm_path)]).splitlines()
        if line.strip()
    }
    capabilities = {line.split()[0] for line in dependency_lines}
    if not REQUIRED_DEPENDENCIES.issubset(capabilities) or not (
        REQUIRED_DEPENDENCY_LINES <= dependency_lines
    ):
        fail("required RHEL dependency is missing")
    for dependency in dependency_lines:
        capability = dependency.split()[0]
        match = OLD_ICU.search(dependency)
        if "--nodeps" in dependency or (match and int(match.group(1)) < 67):
            fail("unsafe dependency or ICU compatibility alias")
        if dependency in REQUIRED_DEPENDENCY_LINES:
            continue
        if dependency in ALLOWED_EXACT_AUTO_DEPENDENCIES:
            continue
        if dependency in ALLOWED_DEPENDENCY_CAPABILITIES:
            continue
        if " " not in dependency and ALLOWED_AUTO_DEPENDENCY.fullmatch(capability):
            continue
        if capability in ALLOWED_DEPENDENCY_CAPABILITIES:
            fail("unapproved RPM dependency constraint")
        else:
            fail("unapproved RPM dependency")

    post_requirements = []
    requirement_metadata = run_query(
        [
            "rpm",
            "-qp",
            "--qf",
            "[%{REQUIRENAME}\t%{REQUIREFLAGS}\t%{REQUIREVERSION}\n]",
            str(rpm_path),
        ]
    )
    for line in requirement_metadata.splitlines():
        fields = line.split("\t")
        if len(fields) != 3:
            fail("RPM dependency metadata is invalid")
        name, flags, version = fields
        if name == SYSTEMD_TMPFILES_POST_REQUIRE[0]:
            try:
                numeric_flags = int(flags)
            except ValueError:
                fail("RPM dependency metadata is invalid")
            post_requirements.append((name, numeric_flags, version))
    if post_requirements != [SYSTEMD_TMPFILES_POST_REQUIRE]:
        fail("systemd-tmpfiles postinstall dependency metadata is invalid")


def verify_scripts(rpm_path):
    scripts = run_query(["rpm", "-qp", "--scripts", str(rpm_path)])
    if len(scripts) > 256 * 1024:
        fail("RPM script metadata exceeds the verification limit")
    if (
        TAPE_SCRIPT.search(scripts)
        or DEVICE_SCRIPT.search(scripts)
        or scripts != EXPECTED_SCRIPTS
    ):
        fail("package script is outside the exact allowlist")


class CpioReader:
    def __init__(self, stream, timeout):
        self.stream = stream
        self.descriptor = stream.fileno()
        self.deadline = time.monotonic() + timeout
        self.total = 0

    def read_exact(self, length):
        if length < 0 or self.total + length > MAX_CPIO_STREAM_SIZE:
            fail("RPM payload stream exceeds the verification limit")
        result = bytearray()
        while len(result) < length:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                fail("RPM payload extraction timed out")
            readable, _, _ = select.select([self.descriptor], [], [], remaining)
            if not readable:
                fail("RPM payload extraction timed out")
            block = os.read(self.descriptor, min(64 * 1024, length - len(result)))
            if not block:
                fail("RPM payload stream is truncated")
            result.extend(block)
            self.total += len(block)
        return bytes(result)

    def copy_exact(self, length, target):
        checksum = 0
        remaining = length
        while remaining:
            block = self.read_exact(min(64 * 1024, remaining))
            target.write(block)
            checksum = (checksum + sum(block)) & 0xFFFFFFFF
            remaining -= len(block)
        return checksum

    def consume_padding(self, length):
        padding = (-length) % 4
        if padding and self.read_exact(padding) != b"\0" * padding:
            fail("RPM payload stream padding is invalid")

    def consume_trailing_padding(self):
        while True:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                fail("RPM payload extraction timed out")
            readable, _, _ = select.select([self.descriptor], [], [], remaining)
            if not readable:
                fail("RPM payload extraction timed out")
            allowance = MAX_CPIO_STREAM_SIZE - self.total
            if allowance <= 0:
                fail("RPM payload stream exceeds the verification limit")
            block = os.read(self.descriptor, min(64 * 1024, allowance))
            if not block:
                return
            self.total += len(block)
            if any(block):
                fail("RPM payload stream has data after its trailer")


def parse_cpio_header(header):
    if header[:6] not in (b"070701", b"070702"):
        fail("RPM payload is not a supported cpio stream")
    try:
        fields = tuple(
            int(header[offset : offset + 8], 16) for offset in range(6, 110, 8)
        )
    except ValueError:
        fail("RPM payload cpio header is invalid")
    return header[:6], fields


def normalized_cpio_path(encoded_name):
    if not encoded_name.endswith(b"\0") or b"\0" in encoded_name[:-1]:
        fail("RPM payload cpio path is invalid")
    try:
        name = encoded_name[:-1].decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        fail("RPM payload cpio path is invalid")
    name = name.removeprefix("./")
    components = name.split("/")
    if (
        not name
        or name.startswith("/")
        or any(component in ("", ".", "..") for component in components)
        or any(ord(character) < 32 for character in name)
    ):
        fail("RPM payload cpio path is unsafe")
    return "/" + "/".join(components), components


def ensure_safe_parent(root, components):
    parent = root
    for component in components:
        parent /= component
        try:
            parent.mkdir(mode=0o700)
        except FileExistsError:
            if parent.is_symlink() or not parent.is_dir():
                fail("RPM payload cpio parent path is unsafe")
    return parent


def safe_symlink_target(rpm_path, entry_components, encoded_target):
    try:
        target = encoded_target.decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        fail("RPM payload symlink target is invalid")
    candidate = PurePosixPath(target)
    if candidate.is_absolute() or any(ord(character) < 32 for character in target):
        fail("RPM payload symlink escapes the extraction root")
    depth = len(entry_components) - 1
    for component in candidate.parts:
        if component in ("", "."):
            continue
        if component == "..":
            depth -= 1
            if depth < 0:
                fail("RPM payload symlink escapes the extraction root")
        else:
            depth += 1
    if EXPECTED_SYMLINKS.get(rpm_path) != target:
        fail("RPM payload symlink target is outside the exact allowlist")
    return target


def extract_cpio_stream(reader, destination, records):
    seen = set()
    total_size = 0
    count = 0
    while True:
        magic, fields = parse_cpio_header(reader.read_exact(110))
        mode = fields[1]
        file_size = fields[6]
        name_size = fields[11]
        expected_checksum = fields[12]
        if name_size < 1 or name_size > MAX_PAYLOAD_PATH_SIZE + 1:
            fail("RPM payload cpio path exceeds the verification limit")
        encoded_name = reader.read_exact(name_size)
        reader.consume_padding(110 + name_size)
        if encoded_name == b"TRAILER!!!\0":
            if file_size != 0:
                fail("RPM payload cpio trailer is invalid")
            reader.consume_trailing_padding()
            return

        count += 1
        if count > MAX_PAYLOAD_ENTRIES:
            fail("RPM payload has too many entries")
        rpm_path, components = normalized_cpio_path(encoded_name)
        if rpm_path in seen or rpm_path not in records:
            fail("RPM payload cpio path differs from RPM metadata")
        seen.add(rpm_path)

        file_type = stat.S_IFMT(mode)
        if file_type not in (stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK):
            fail("RPM payload cpio entry type is unsafe")
        expected_mode = records[rpm_path]
        if expected_mode is not None and stat.S_IFMT(expected_mode[0]) != file_type:
            fail("RPM payload cpio entry type differs from RPM metadata")
        if file_type == stat.S_IFDIR and file_size != 0:
            fail("RPM payload cpio directory has data")
        if file_type == stat.S_IFLNK and file_size > MAX_PAYLOAD_PATH_SIZE:
            fail("RPM payload symlink target exceeds the verification limit")
        if file_type == stat.S_IFREG and file_size > MAX_PAYLOAD_FILE_SIZE:
            fail("RPM payload file exceeds the verification limit")
        total_size += file_size
        if total_size > MAX_PAYLOAD_SCAN_SIZE:
            fail("RPM payload exceeds the aggregate verification limit")

        parent = ensure_safe_parent(destination, components[:-1])
        target = parent / components[-1]
        if file_type == stat.S_IFDIR:
            try:
                target.mkdir(mode=0o700)
            except FileExistsError:
                if target.is_symlink() or not target.is_dir():
                    fail("RPM payload cpio directory path is unsafe")
            checksum = 0
        elif file_type == stat.S_IFLNK:
            content = reader.read_exact(file_size)
            checksum = sum(content) & 0xFFFFFFFF
            link_target = safe_symlink_target(rpm_path, components, content)
            try:
                target.symlink_to(link_target)
            except FileExistsError:
                fail("RPM payload cpio path is duplicated")
        else:
            try:
                descriptor = os.open(
                    target,
                    os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                    0o600,
                )
            except OSError:
                fail("RPM payload cpio file path is unsafe")
            with os.fdopen(descriptor, "wb") as output:
                checksum = reader.copy_exact(file_size, output)
        reader.consume_padding(file_size)
        if magic == b"070702" and checksum != expected_checksum:
            fail("RPM payload cpio checksum is invalid")


def extract_payload(rpm_path, destination, records):
    try:
        producer = subprocess.Popen(
            ["rpm2cpio", str(rpm_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        fail("RPM payload extraction tool is unavailable")
    assert producer.stdout is not None
    try:
        reader = CpioReader(producer.stdout, 30)
        extract_cpio_stream(reader, destination, records)
        producer.wait(timeout=max(0.01, reader.deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        terminate_process(producer)
        fail("RPM payload extraction timed out")
    except VerificationError:
        terminate_process(producer)
        raise
    except OSError:
        terminate_process(producer)
        fail("RPM payload extraction failed")
    if producer.returncode != 0:
        fail("RPM payload extraction failed")


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def source_manifest_record(archive_path):
    if (
        not archive_path.is_file()
        or archive_path.is_symlink()
        or archive_path.name != "lto-ltfs-0.1.0.tar.gz"
    ):
        fail("source archive path is invalid")
    with tempfile.TemporaryDirectory(prefix="lto-ltfs-source-manifest-") as temporary:
        extract_source_archive(archive_path, Path(temporary))
    return {
        "schema": 1,
        "archive_name": archive_path.name,
        "archive_sha256": file_sha256(archive_path),
    }


def write_source_manifest(archive_path, manifest_path):
    if manifest_path.exists() or manifest_path.is_symlink():
        fail("source manifest output must be a new path")
    record = source_manifest_record(archive_path)
    manifest_path.write_text(
        json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n",
        encoding="utf-8",
    )


def read_source_manifest(manifest_path):
    try:
        descriptor = os.open(
            manifest_path,
            os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK,
        )
    except OSError:
        fail("trusted source manifest path is invalid")
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            fail("trusted source manifest path is invalid")
        if metadata.st_size > MAX_SOURCE_MANIFEST_SIZE:
            fail("trusted source manifest exceeds the verification limit")
        source = os.fdopen(descriptor, "rb")
        descriptor = -1
        with source:
            raw = source.read(MAX_SOURCE_MANIFEST_SIZE + 1)
        if len(raw) > MAX_SOURCE_MANIFEST_SIZE:
            fail("trusted source manifest exceeds the verification limit")
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    try:
        record = json.loads(raw.decode("utf-8", errors="strict"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        fail("trusted source manifest is invalid")
    if not isinstance(record, dict) or set(record) != {
        "schema",
        "archive_name",
        "archive_sha256",
    }:
        fail("trusted source manifest schema is invalid")
    if (
        record["schema"] != 1
        or record["archive_name"] != "lto-ltfs-0.1.0.tar.gz"
        or not isinstance(record["archive_sha256"], str)
        or not re.fullmatch(r"[0-9a-f]{64}", record["archive_sha256"])
    ):
        fail("trusted source manifest values are invalid")
    return record


def verify_source_manifest(archive_path, manifest_path):
    expected = read_source_manifest(manifest_path)
    if file_sha256(archive_path) != expected["archive_sha256"]:
        fail("SRPM source archive differs from trusted source manifest")


def read_source_file_names(rpm_path):
    output = run_query(["rpm", "-qp", "--qf", "[%{FILENAMES}\\n]", str(rpm_path)])
    names = output.splitlines()
    expected = {
        "lto-ltfs-0.1.0.tar.gz",
        "lto-ltfs.spec",
        "99-lto-ltfs.rules",
        "lto-ltfs.conf",
    }
    if len(names) != len(set(names)) or set(names) != expected:
        fail("SRPM source list is outside the exact allowlist")
    return expected


def extract_source_archive(archive_path, destination):
    try:
        with tarfile.open(archive_path, mode="r:gz") as archive:
            extract_source_archive_members(archive, destination)
    except (OSError, tarfile.TarError):
        fail("SRPM source archive is invalid")


def extract_source_archive_members(archive, destination):
    expected_prefix = PurePosixPath("lto-ltfs-0.1.0")
    total_size = 0
    count = 0
    seen = set()
    for member in archive:
        count += 1
        if count > 100000:
            fail("SRPM source archive has too many entries")
        candidate = PurePosixPath(member.name)
        if (
            candidate.is_absolute()
            or ".." in candidate.parts
            or not candidate.parts
            or candidate.parts[0] != expected_prefix.name
            or member.issym()
            or member.islnk()
            or not (member.isfile() or member.isdir())
            or candidate in seen
        ):
            fail("SRPM source archive contains an unsafe entry")
        seen.add(candidate)
        if member.size > MAX_PAYLOAD_FILE_SIZE:
            fail("SRPM source archive file exceeds the verification limit")
        total_size += member.size
        if total_size > MAX_PAYLOAD_SCAN_SIZE:
            fail("SRPM source archive exceeds the verification limit")
        target = destination.joinpath(*candidate.parts)
        if member.isdir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        source = archive.extractfile(member)
        if source is None:
            fail("SRPM source archive entry cannot be read")
        data = source.read(MAX_PAYLOAD_FILE_SIZE + 1)
        if len(data) != member.size:
            fail("SRPM source archive entry length is invalid")
        target.write_bytes(data)


def verify_source_rpm(rpm_path, source_manifest):
    if (
        not rpm_path.is_file()
        or rpm_path.is_symlink()
        or not rpm_path.name.endswith(".src.rpm")
    ):
        fail("SRPM path must name one regular source package")
    for tool in ("rpm", "rpm2cpio"):
        if shutil.which(tool) is None:
            fail("required RPM verification tool is unavailable")
    read_metadata(rpm_path, source=True)
    expected_sources = read_source_file_names(rpm_path)
    with tempfile.TemporaryDirectory(prefix="lto-ltfs-srpm-verify-") as temporary:
        payload_root = Path(temporary) / "payload"
        source_root = Path(temporary) / "source"
        payload_root.mkdir()
        source_root.mkdir()
        source_records = {"/" + name: None for name in expected_sources}
        extract_payload(rpm_path, payload_root, source_records)
        for item in payload_root.rglob("*"):
            if item.is_symlink() or not (item.is_file() or item.is_dir()):
                fail("SRPM payload contains an unsafe source entry")
        actual = {
            item.relative_to(payload_root).as_posix()
            for item in payload_root.rglob("*")
            if item.is_file()
        }
        if actual != expected_sources:
            fail("extracted SRPM differs from its source metadata")
        source_archive = payload_root / "lto-ltfs-0.1.0.tar.gz"
        verify_source_manifest(source_archive, source_manifest)
        extract_source_archive(source_archive, source_root)
        project_root = source_root / "lto-ltfs-0.1.0"
        for packaged_name, project_name in (
            ("lto-ltfs.spec", "packaging/rpm/lto-ltfs.spec"),
            ("99-lto-ltfs.rules", "packaging/udev/99-lto-ltfs.rules"),
            ("lto-ltfs.conf", "packaging/tmpfiles/lto-ltfs.conf"),
        ):
            if (payload_root / packaged_name).read_bytes() != (
                project_root / project_name
            ).read_bytes():
                fail("SRPM source does not match the provenance archive")


def extracted_payload_paths(root):
    paths = set()
    root_resolved = root.resolve()
    for item in root.rglob("*"):
        relative = item.relative_to(root)
        rpm_path = "/" + relative.as_posix()
        if item.is_symlink():
            target = Path(os.readlink(item))
            if (
                target.is_absolute()
                or root_resolved not in (item.parent / target).resolve().parents
            ):
                fail("RPM payload symlink escapes the extraction root")
            paths.add(rpm_path)
        elif item.is_file():
            paths.add(rpm_path)
        elif not item.is_dir():
            fail("RPM payload contains an extracted special file")
    return paths


def is_canonical_version_context(path, text, match):
    if match.group() != "2.4.8.4":
        return False
    line_start = text.rfind("\n", 0, match.start()) + 1
    line_end = text.find("\n", match.end())
    if line_end < 0:
        line_end = len(text)
    line = text[line_start:line_end]
    if path == CONFIG_HEADER_PATH:
        return line in CANONICAL_CONFIG_HEADER_LINES
    if path == "/usr/lib64/pkgconfig/ltfs.pc":
        return line == "Version: " + CANONICAL_PRODUCT_VERSION
    if path == "/usr/share/licenses/lto-ltfs/upstream.json":
        canonical_line = f'"tag": "{CANONICAL_UPSTREAM_TAG}"'
        return line.strip() in {canonical_line, canonical_line + ","}
    if path == CORE_LIBRARY_PATH:
        immediate = text[match.start() - 2 : match.end() + 4]
        if immediate in {
            "\x48\xbe2.4.8.4 \x48\x89\x30",
            "\x48\xb82.4.8.4 \x48\x89\x43",
        }:
            return True
    if path not in EXPECTED_BINARY_PATHS:
        return False
    field_delimiter = text.rfind("\0", 0, match.start())
    if field_delimiter < 0:
        return False
    field_start = field_delimiter + 1
    field_end = text.find("\0", match.end())
    if field_end < 0:
        return False
    return text[field_start:field_end] == CANONICAL_PRODUCT_VERSION


def is_canonical_serial_shaped_context(path, text, match):
    if path != TRACE_HEADER_PATH or match.group() != "F0000000":
        return False
    line_start = text.rfind("\n", 0, match.start()) + 1
    line_end = text.find("\n", match.end())
    if line_end < 0:
        line_end = len(text)
    return text[line_start:line_end] == TRACE_STATUS_MASK_LINE


def verify_hpe_compatibility(path, text):
    if path == LGPL_NOTICE_PATH:
        if hashlib.sha256(text.encode("latin-1")).hexdigest() != LGPL_NOTICE_SHA256:
            fail("LGPL notice differs from reviewed source")
        return
    if path == "/usr/bin/ltfs-info":
        # The capacity diagnostic recognizes the exact SCSI inquiry vendor
        # identifier. This is not a general exception for vendor payloads.
        segments = text.split("\0")
        fields = tuple(field for field in segments[1:-1] if HPE_MARKER.search(field))
        if (HPE_MARKER.search(segments[0]) or HPE_MARKER.search(segments[-1])
                or fields != ("HPE",)):
            fail("LTFS info vendor field contract is invalid")
        return
    expected_lines = HPE_HEADER_LINES_BY_PATH.get(path)
    if expected_lines is not None:
        actual_lines = tuple(
            line for line in text.split("\n") if HPE_MARKER.search(line)
        )
        if len(actual_lines) != len(expected_lines) or any(
            actual_lines.count(line) != expected_lines.count(line)
            for line in expected_lines
        ):
            fail("HPE compatibility header contract is invalid")
        return
    if path == SG_PLUGIN_PATH:
        segments = text.split("\0")
        if HPE_MARKER.search(segments[0]) or HPE_MARKER.search(segments[-1]):
            fail("HPE SG compatibility field contract is invalid")
        actual_fields = tuple(
            field for field in segments[1:-1] if HPE_MARKER.search(field)
        )
        if not any(
            len(actual_fields) == len(expected)
            and all(
                actual_fields.count(field) == expected.count(field)
                for field in set(expected)
            )
            for expected in (HPE_SG_FIELDS, HPE_SG_OPTIMIZED_FIELDS)
        ):
            fail("HPE SG compatibility field contract is invalid")
        return
    if HPE_MARKER.search(text):
        fail("forbidden vendor or JRE content in RPM payload")


def verify_text(path, text):
    if path == LICENSE_INVENTORY_PATH:
        # This disclosure necessarily names the compared vendor and retained
        # evidence. Its exact bytes are pinned; no generic payload exception.
        if hashlib.sha256(text.encode("latin-1")).hexdigest() != LICENSE_INVENTORY_SHA256:
            fail("installed license inventory differs from reviewed source")
        return
    if path == LGPL_TEXT_PATH and (
        hashlib.sha256(text.encode("latin-1")).hexdigest() != LGPL_TEXT_SHA256
    ):
        fail("LGPL license text differs from retained license")
    if FORBIDDEN_PRODUCT.search(text):
        fail("forbidden vendor or JRE content in RPM payload")
    verify_hpe_compatibility(path, text)
    if SECRET.search(text):
        fail("secret or private key in RPM payload")
    if PRIVATE_PATH.search(text):
        fail("private source path in RPM payload")
    canonical_serial_matches = 0
    for match in DEVICE_SERIAL.finditer(text):
        if is_canonical_serial_shaped_context(path, text, match):
            canonical_serial_matches += 1
            if canonical_serial_matches > 1:
                fail("production serial is embedded in the RPM payload")
        else:
            fail("production serial is embedded in the RPM payload")
    for match in IPV4.finditer(text):
        if is_canonical_version_context(path, text, match):
            continue
        candidate = match.group()
        try:
            address = ipaddress.ip_address(candidate)
        except ValueError:
            continue
        if not (
            address.is_loopback
            or address.is_unspecified
            or any(address in network for network in DOCUMENTATION_NETWORKS)
        ):
            fail("non-test IP address in RPM payload")
    if path == "/etc/lto-ltfs/device.json":
        try:
            device = json.loads(text)
        except json.JSONDecodeError:
            fail("device selector JSON is invalid")
        if set(device) != {"nst_path", "sg_path", "serial", "wwid"}:
            fail("device selector schema is invalid")
        if device != {
            "nst_path": "__UNPROVISIONED__",
            "sg_path": "__UNPROVISIONED__",
            "serial": "__UNPROVISIONED__",
            "wwid": "__UNPROVISIONED__",
        }:
            fail("production serial or WWID is embedded in the RPM")


def verify_config_header(text):
    lines = text.split("\n")
    if any(lines.count(expected) != 1 for expected in CANONICAL_CONFIG_HEADER_LINES):
        fail("installed LTFS configuration header version contract is invalid")


def verify_extracted_payload(root, records):
    actual_paths = extracted_payload_paths(root)
    metadata_paths = {
        path
        for path, (mode, _owner, _group) in records.items()
        if not stat.S_ISDIR(mode)
    }
    if actual_paths != metadata_paths:
        fail("extracted payload differs from RPM file metadata")
    total_scanned = 0
    for rpm_path in sorted(actual_paths):
        source = root / rpm_path.lstrip("/")
        if source.is_symlink():
            continue
        size = source.stat().st_size
        if size > MAX_PAYLOAD_FILE_SIZE:
            fail("RPM payload file exceeds the verification limit")
        total_scanned += size
        if total_scanned > MAX_PAYLOAD_SCAN_SIZE:
            fail("RPM payload exceeds the aggregate verification limit")
        payload = source.read_bytes()
        text = payload.decode("latin-1")
        verify_text(rpm_path, text)
        if rpm_path == CONFIG_HEADER_PATH:
            verify_config_header(text)

    udev = (root / "usr/lib/udev/rules.d/99-lto-ltfs.rules").read_text()
    udev_entries = [
        line for line in udev.splitlines()
        if line and not line.startswith("#")
    ]
    expected_udev = [
        (
            'ACTION=="add", SUBSYSTEM=="scsi_generic", KERNEL=="sg[0-9]*", '
            'ATTRS{type}=="1", ENV{ID_SERIAL}=="", '
            'IMPORT{program}="/usr/lib/udev/scsi_id --export --whitelisted '
            '--device=$devnode"'
        ),
        (
            'ACTION=="add", SUBSYSTEM=="scsi_generic", KERNEL=="sg[0-9]*", '
            'ATTRS{type}=="1", ENV{ID_SERIAL}!="", '
            'SYMLINK+="lto-archiver-scsi-$env{ID_SERIAL}", '
            'GROUP="lto-admin", MODE="0660", '
            'OPTIONS+="string_escape=replace"'
        ),
    ]
    if udev_entries != expected_udev:
        fail("udev rule is not tape-class and serial bound")
    links = re.findall(r'SYMLINK\+="([^"]+)"', udev_entries[1])
    if len(links) != 1:
        fail("udev rule does not own one stable alias")
    link_path = PurePosixPath(links[0])
    if (
        link_path.is_absolute()
        or "/" in links[0]
        or (PurePosixPath("/dev") / link_path).parent != PurePosixPath("/dev")
    ):
        fail("udev rule does not own one flat /dev alias")
    tmpfiles = (root / "usr/lib/tmpfiles.d/lto-ltfs.conf").read_text().splitlines()
    entries = [line for line in tmpfiles if line and not line.startswith("#")]
    if entries != ["d /run/lock/lto-ltfs 0770 root lto-admin -"]:
        fail("tmpfiles policy is not the closed runtime-directory contract")


def verify(rpm_path):
    if not rpm_path.is_file() or rpm_path.is_symlink() or rpm_path.suffix != ".rpm":
        fail("RPM path must name one regular package")
    for tool in ("rpm", "rpm2cpio"):
        if shutil.which(tool) is None:
            fail("required RPM verification tool is unavailable")
    read_metadata(rpm_path)
    records = read_file_records(rpm_path)
    verify_files(records)
    verify_dependencies(rpm_path)
    verify_scripts(rpm_path)
    with tempfile.TemporaryDirectory(prefix="lto-ltfs-rpm-verify-") as temporary:
        root = Path(temporary)
        extract_payload(rpm_path, root, records)
        verify_extracted_payload(root, records)


def main(argv):
    write_manifest = len(argv) == 4 and argv[1] == "--write-source-manifest"
    source = len(argv) >= 2 and argv[1] == "--srpm"
    if write_manifest:
        try:
            write_source_manifest(Path(argv[2]), Path(argv[3]))
        except (VerificationError, OSError) as error:
            print("Source manifest creation failed: " + str(error), file=sys.stderr)
            return 1
        print("Source manifest created")
        return 0
    if source:
        valid = len(argv) == 5 and argv[3] == "--source-manifest"
    else:
        valid = len(argv) == 2
    if not valid:
        print(
            "usage: verify-rpm.py PACKAGE.rpm | "
            "--srpm PACKAGE.src.rpm --source-manifest MANIFEST | "
            "--write-source-manifest ARCHIVE MANIFEST",
            file=sys.stderr,
        )
        return 2
    try:
        package = Path(argv[2] if source else argv[1])
        if source:
            verify_source_rpm(package, Path(argv[4]))
        else:
            verify(package)
    except (VerificationError, OSError) as error:
        print("RPM verification failed: " + str(error), file=sys.stderr)
        return 1
    print(("SRPM" if source else "RPM") + " verification passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
