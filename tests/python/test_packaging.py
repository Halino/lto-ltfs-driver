import gzip
import hashlib
import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import unittest
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
README = ROOT / "README.md"
SPEC = ROOT / "packaging/rpm/lto-ltfs.spec"
UDEV = ROOT / "packaging/udev/99-lto-ltfs.rules"
TMPFILES = ROOT / "packaging/tmpfiles/lto-ltfs.conf"
VERIFY = ROOT / "scripts/verify-rpm.py"
BUILD = ROOT / "scripts/build-rpm.sh"
RPM_CONTAINERFILE = ROOT / "packaging/rpm/Containerfile"
PRUNE_BUILDROOT = ROOT / "packaging/rpm/prune-buildroot.sh"
TAPE_SOURCE = ROOT / "src/libltfs/tape.c"
TAPE_HEADER = ROOT / "src/libltfs/tape_ops.h"
LTFS_HEADER = ROOT / "src/libltfs/ltfs.h"
FUSE_SOURCE = ROOT / "src/ltfs_fuse.c"
LTFS_INFO_SOURCE = ROOT / "src/utils/ltfs_info.c"
LTFS_INFO_SELF_TEST = {
    "schema": 2,
    "media_state": "ltfs",
    "tape_by_id": "/dev/tape/by-id/self-test-nst",
    "scsi_by_id": "/dev/lto-archiver-scsi-self-test-sg",
    "drive_serial": "SELFTEST",
    "mam_barcode": "SELFTEST01",
    "mam_volume_serial": "SELFTEST-SERIAL-01",
    "ltfs_volume_label": "SELF TEST",
    "ltfs_volume_uuid": "11111111-2222-3333-4444-555555555555",
    "index_generation": 1,
}
RAW_MAM_IDENTITY_EXPRESSIONS = (
    "t_attr->volume_identifier",
    "t_attr->medium_serial_number",
    "t_attr->medium_label",
    "t_attr->barcode",
)


def info_log_raw_identity_expressions(loader):
    calls = re.findall(r"\bltfsmsg\s*\(\s*LTFS_INFO\b.*?\);", loader, re.DOTALL)
    return {
        expression
        for expression in RAW_MAM_IDENTITY_EXPRESSIONS
        if any(expression in call for call in calls)
    }


REAL_RHEL9_REQUIRES = (
    "/bin/sh\n"
    "/usr/bin/fusermount\n"
    "/usr/bin/systemd-tmpfiles\n"
    "/usr/lib/udev/scsi_id\n"
    "/usr/bin/pkg-config\n"
    "fuse-libs >= 2.9.9\n"
    "libc.so.6()(64bit)\n"
    "libc.so.6(GLIBC_2.14)(64bit)\n"
    "libc.so.6(GLIBC_2.17)(64bit)\n"
    "libc.so.6(GLIBC_2.2.5)(64bit)\n"
    "libc.so.6(GLIBC_2.28)(64bit)\n"
    "libc.so.6(GLIBC_2.3)(64bit)\n"
    "libc.so.6(GLIBC_2.3.2)(64bit)\n"
    "libc.so.6(GLIBC_2.3.3)(64bit)\n"
    "libc.so.6(GLIBC_2.32)(64bit)\n"
    "libc.so.6(GLIBC_2.33)(64bit)\n"
    "libc.so.6(GLIBC_2.34)(64bit)\n"
    "libc.so.6(GLIBC_2.4)(64bit)\n"
    "libc.so.6(GLIBC_2.7)(64bit)\n"
    "libcrypto.so.3()(64bit)\n"
    "libcrypto.so.3(OPENSSL_3.0.0)(64bit)\n"
    "libfuse.so.2()(64bit)\n"
    "libfuse.so.2(FUSE_2.2)(64bit)\n"
    "libfuse.so.2(FUSE_2.5)(64bit)\n"
    "libfuse.so.2(FUSE_2.6)(64bit)\n"
    "libfuse.so.2(FUSE_2.8)(64bit)\n"
    "libicu\n"
    "libicudata.so.67()(64bit)\n"
    "libicui18n.so.67()(64bit)\n"
    "libicuuc.so.67()(64bit)\n"
    "libltfs.so.0()(64bit)\n"
    "libuuid\n"
    "libuuid.so.1()(64bit)\n"
    "libuuid.so.1(UUID_1.0)(64bit)\n"
    "libxml2\n"
    "libxml2.so.2()(64bit)\n"
    "libxml2.so.2(LIBXML2_2.4.30)(64bit)\n"
    "libxml2.so.2(LIBXML2_2.5.0)(64bit)\n"
    "libxml2.so.2(LIBXML2_2.6.0)(64bit)\n"
    "libxml2.so.2(LIBXML2_2.6.15)(64bit)\n"
    "libxml2.so.2(LIBXML2_2.6.28)(64bit)\n"
    "libxml2.so.2(LIBXML2_2.6.5)(64bit)\n"
    "openssl-libs\n"
    "rpmlib(CompressedFileNames) <= 3.0.4-1\n"
    "rpmlib(FileDigests) <= 4.6.0-1\n"
    "rpmlib(PayloadFilesHavePrefix) <= 4.0-1\n"
    "shadow-utils\n"
    "zlib\n"
)


class PackagingPolicyTests(unittest.TestCase):
    def test_ltfs_info_opens_sg_for_read_attribute_without_rawio_capability(self):
        source = LTFS_INFO_SOURCE.read_text(encoding="utf-8")
        self.assertIn(
            "O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW", source
        )
        self.assertNotIn(
            "O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW", source
        )

    def test_runtime_container_executes_closed_ltfs_info_fixture_contract(self):
        containerfile = RPM_CONTAINERFILE.read_text(encoding="utf-8")
        runtime = containerfile.split(" AS runtime-test\n", 1)[1]
        start = runtime.index("ltfs-info --self-test-fixture ready")
        end = runtime.index("&& rpm -V lto-ltfs", start)
        validation = runtime[start:end].replace("\\\n", " ")

        def run_validation(payload):
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                fixture_path = root / "fixture.json"
                binary = root / "ltfs-info"
                binary.write_text(
                    "#!/bin/sh\n"
                    "test \"$1\" = --self-test-fixture\n"
                    "test \"$2\" = ready\n"
                    "printf '%s\\n' \"$LTFS_INFO_TEST_PAYLOAD\"\n",
                    encoding="utf-8",
                )
                binary.chmod(0o700)
                command = validation.replace(
                    "/tmp/ltfs-info-fixture.json", shlex.quote(str(fixture_path))
                )
                environment = os.environ.copy()
                environment["PATH"] = str(root) + os.pathsep + environment["PATH"]
                environment["LTFS_INFO_TEST_PAYLOAD"] = json.dumps(
                    payload, separators=(",", ":")
                )
                return subprocess.run(
                    ["sh", "-c", command],
                    text=True,
                    capture_output=True,
                    env=environment,
                    check=False,
                )

        self.assertEqual(run_validation(LTFS_INFO_SELF_TEST).returncode, 0)
        for mutation in (
            {"ready": True},
            {**LTFS_INFO_SELF_TEST, "extra": True},
            dict(reversed(LTFS_INFO_SELF_TEST.items())),
            {
                key: value
                for key, value in LTFS_INFO_SELF_TEST.items()
                if key != "ltfs_volume_uuid"
            },
        ):
            with self.subTest(mutation=mutation):
                self.assertNotEqual(run_validation(mutation).returncode, 0)

    def test_medium_serial_number_is_read_only_physical_identity(self):
        source = TAPE_SOURCE.read_text(encoding="utf-8")
        header = TAPE_HEADER.read_text(encoding="utf-8")
        ltfs_header = LTFS_HEADER.read_text(encoding="utf-8")
        fuse_source = FUSE_SOURCE.read_text(encoding="utf-8")
        setter = source.split("int tape_set_attribute_to_cm", 1)[1].split(
            "int tape_get_attribute_from_cm", 1
        )[0]
        getter = source.split("int tape_get_attribute_from_cm", 1)[1].split(
            "void tape_load_all_attribute_from_cm", 1
        )[0]
        loader = source.split("void tape_load_all_attribute_from_cm", 1)[1].split(
            "int update_tape_attribute", 1
        )[0]
        receipt = fuse_source.split("if (priv->standalone_receipt)", 1)[1].split(
            "priv->mount_init_result = ltfs_emit_process_event", 1
        )[0]
        self.assertIn("TC_MAM_MEDIUM_SERIAL_NUMBER (0x0401)", header)
        self.assertIn("medium_serial_number", ltfs_header)
        self.assertNotIn("TC_MAM_VOLUME_IDENTIFIER", setter)
        self.assertNotIn("TC_MAM_MEDIUM_SERIAL_NUMBER", setter)
        self.assertIn("TC_MAM_MEDIUM_SERIAL_NUMBER", getter)
        self.assertIn("TC_MAM_MEDIUM_SERIAL_NUMBER", loader)
        self.assertIn("attributes->medium_serial_number", receipt)
        self.assertNotIn("attributes->volume_identifier", receipt)

    def test_raw_mam_identity_values_are_not_info_logged(self):
        loader = TAPE_SOURCE.read_text(encoding="utf-8").split(
            "void tape_load_all_attribute_from_cm", 1
        )[1].split("int update_tape_attribute", 1)[0]
        self.assertEqual(info_log_raw_identity_expressions(loader), set())

        for expression in RAW_MAM_IDENTITY_EXPRESSIONS:
            with self.subTest(expression=expression):
                mutated_loader = (
                    loader
                    + "\nltfsmsg(LTFS_INFO, 17227I, \"raw identity\",\n"
                    + f"\t{expression});\n"
                )
                self.assertEqual(
                    info_log_raw_identity_expressions(mutated_loader),
                    {expression},
                )

    def test_readme_rpm_verification_examples_use_release_22(self):
        readme = README.read_text(encoding="utf-8")
        self.assertEqual(
            readme.count("lto-ltfs-0.1.0-22.el9.x86_64.rpm"), 1
        )
        self.assertEqual(readme.count("lto-ltfs-0.1.0-22.el9.src.rpm"), 1)
        self.assertNotRegex(
            readme,
            r"lto-ltfs-0\.1\.0-(?:[1-9]|1[0-9]|20)\.el9\.(?:x86_64|src)\.rpm",
        )

    def test_buildroot_pruning_removes_only_the_exact_unused_install_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            buildroot = Path(temporary)
            expected = buildroot / "usr/bin/ltfs"
            retained = buildroot / "usr/share/example/retained.txt"
            unexpected = (
                buildroot / "usr/share/ltfs/ltfs",
                buildroot / "usr/share/snmp/LTFS-MIB.txt",
                buildroot / "usr/share/snmp/LtfsSnmpTrapDef.txt",
            )
            for path in (expected, retained, *unexpected):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(path.name, encoding="utf-8")

            result = subprocess.run(
                ["sh", str(PRUNE_BUILDROOT), str(buildroot)],
                capture_output=True,
                text=True,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            actual_files = {
                path.relative_to(buildroot).as_posix()
                for path in buildroot.rglob("*")
                if path.is_file()
            }
            self.assertEqual(
                actual_files,
                {"usr/bin/ltfs", "usr/share/example/retained.txt"},
            )

    def test_spec_is_reproducible_runs_tests_and_has_closed_runtime_dependencies(self):
        spec = SPEC.read_text(encoding="utf-8")
        self.assertIn("%global _buildhost reproducible", spec)
        self.assertIn("%global use_source_date_epoch_as_buildtime 1", spec)
        self.assertIn("%global clamp_mtime_to_source_date_epoch 1", spec)
        self.assertNotIn("%global _use_source_date_epoch_as_buildtime", spec)
        self.assertNotIn("%global _build_mtime_policy", spec)
        self.assertIn("%check\nmake check", spec)
        self.assertIn("Release:        22%{?dist}", spec)
        self.assertIn("License:        BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only", spec)
        self.assertIn("- 0.1.0-22", spec)
        self.assertIn("- 0.1.0-21", spec)
        self.assertIn("- 0.1.0-20", spec)
        self.assertIn("- 0.1.0-19", spec)
        self.assertIn("- 0.1.0-18", spec)
        self.assertIn("- 0.1.0-17", spec)
        self.assertIn("- 0.1.0-16", spec)
        self.assertIn(
            "%configure --enable-fast --enable-tests --disable-snmp --disable-lintape",
            spec,
        )
        self.assertIn("%license LICENSE NOTICES COPYING.LIB LGPL-NOTICE provenance/upstream.json", spec)
        for dependency in (
            "/usr/bin/fusermount",
            "/usr/lib/udev/scsi_id",
            "fuse-libs >= 2.9.9",
            "libxml2",
            "libuuid",
            "libicu",
            "openssl-libs",
            "zlib",
        ):
            self.assertIn("Requires:       " + dependency, spec)
        self.assertIn("Requires(post): /usr/bin/systemd-tmpfiles", spec)
        for build_dependency in ("git-core", "libicu", "libubsan", "systemd-rpm-macros"):
            self.assertIn("BuildRequires:  " + build_dependency, spec)
        self.assertNotIn("BuildRequires:  icu\n", spec)
        self.assertNotIn("--nodeps", spec)
        self.assertNotRegex(spec, r"libicu(?:uc|i18n)\.so\.[0-9]+")

    def test_spec_limits_executables_and_protects_mkltfs(self):
        spec = SPEC.read_text(encoding="utf-8")
        self.assertIn("%attr(0750,root,lto-admin) %{_bindir}/mkltfs", spec)
        self.assertNotIn("%{_bindir}/*", spec)
        file_lines = {line.strip() for line in spec.split("%files", 1)[1].splitlines()}
        for executable in ("ltfs", "ltfsck", "mkltfs", "ltfs-info"):
            matching = [
                line for line in file_lines if line.endswith("%{_bindir}/" + executable)
            ]
            self.assertEqual(len(matching), 1)
        scripts = "%pre\n" + spec.split("\n%pre\n", 1)[1].split("\n%files", 1)[0]
        self.assertNotRegex(
            scripts.replace("/dev/null", ""),
            r"(?im)(?:^|[ ;])(?:mkltfs|ltfsck|ltfs|mt|eject|sg_[a-z]+)\b|/dev/",
        )

    def test_message_catalog_install_does_not_duplicate_source_directory(self):
        spec = SPEC.read_text(encoding="utf-8")
        self.assertIn("(cd messages", spec)
        self.assertNotIn("find messages -mindepth", spec)

    def test_installed_hpe_compatibility_contract_is_source_backed(self):
        expected_lines = {
            "src/libltfs/ltfs.h": ("\tNOLOCK_MAM    = 128,  /* From HPE */",),
            "src/libltfs/tape_ops.h": (
                "#define TC_MAM_BARCODE_LEN TC_MAM_BARCODE_SIZE /* HPE LTFS alias */",
                "\t * @param vol_name Volume name, unused by libtlfs (HPE extension)",
                "\t * @param vol_name Volume barcode, unused by libtlfs (HPE extension)",
                "\t * @param vol_mam_uuid Volume UUID, unused by libtlfs (HPE extension)",
            ),
            "src/tape_drivers/ibm_tape.h": (
                (
                    "\tVOLSTATS_USED_CAPACITY    = 0x0203,\t"
                    "/* HPE alias of VOLSTATS_PART_USED_CAP */"
                ),
            ),
        }
        installed_headers = (ROOT / "src/Makefile.am").read_text(encoding="utf-8")
        for relative, lines in expected_lines.items():
            with self.subTest(relative=relative):
                self.assertIn(relative.removeprefix("src/"), installed_headers)
                source_lines = (ROOT / relative).read_text(
                    encoding="utf-8"
                ).splitlines()
                for line in lines:
                    self.assertEqual(source_lines.count(line), 1)

        sg_sources = (ROOT / "src/tape_drivers/linux/sg/Makefile.am").read_text(
            encoding="utf-8"
        )
        self.assertIn("hp_tape.c", sg_sources)
        hp_source = (ROOT / "src/tape_drivers/hp_tape.c").read_text(encoding="utf-8")
        for field in (
            '"HPE LTO - Cleaning Required"',
            '"HPE LTO - Bad microcode detected"',
        ):
            self.assertEqual(hp_source.count(field), 1)

    def test_udev_alias_is_tape_class_serial_bound_and_host_only(self):
        rules = [
            line
            for line in UDEV.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(len(rules), 2)
        import_rule, link_rule = rules
        for clause in (
            'ACTION=="add"',
            'SUBSYSTEM=="scsi_generic"',
            'KERNEL=="sg[0-9]*"',
            'ATTRS{type}=="1"',
            'ENV{ID_SERIAL}==""',
            'IMPORT{program}="/usr/lib/udev/scsi_id --export --whitelisted --device=$devnode"',
        ):
            self.assertIn(clause, import_rule)
        for clause in (
            'ACTION=="add"',
            'SUBSYSTEM=="scsi_generic"',
            'KERNEL=="sg[0-9]*"',
            'ATTRS{type}=="1"',
            'ENV{ID_SERIAL}!=""',
            'SYMLINK+="lto-archiver-scsi-$env{ID_SERIAL}"',
            'GROUP="lto-admin"',
            'MODE="0660"',
        ):
            self.assertIn(clause, link_rule)
        self.assertNotRegex("\n".join(rules).lower(), r"(?:docker|podman|container|webui)")
        serial_literals = [
            token
            for token in link_rule.replace(",", " ").split()
            if "serial" in token.lower() and "$env{ID_SERIAL}" not in token
        ]
        self.assertEqual(serial_literals, ['ENV{ID_SERIAL}!=""'])

        links = re.findall(r'SYMLINK\+="([^"]+)"', link_rule)
        self.assertEqual(links, ["lto-archiver-scsi-$env{ID_SERIAL}"])
        link_path = PurePosixPath(links[0])
        self.assertFalse(link_path.is_absolute())
        self.assertEqual(
            (PurePosixPath("/dev") / link_path).parent,
            PurePosixPath("/dev"),
        )
        self.assertNotIn("/", links[0])

    def test_tmpfiles_creates_only_the_runtime_lock(self):
        policy = TMPFILES.read_text(encoding="utf-8")
        self.assertIn("flat stable SG alias", policy)
        self.assertNotIn("nested stable SG alias", policy)
        entries = [
            line
            for line in policy.splitlines()
            if line and not line.startswith("#")
        ]
        self.assertEqual(
            entries,
            ["d /run/lock/lto-ltfs 0770 root lto-admin -"],
        )
        self.assertNotIn("/dev/", "\n".join(entries))
        spec = SPEC.read_text(encoding="utf-8")
        self.assertEqual(spec.count("\n%post\n"), 1)
        post = spec.split("\n%post\n", 1)[1].split("\n%files", 1)[0]
        self.assertEqual(
            post.strip(),
            "/usr/bin/systemd-tmpfiles --create %{_tmpfilesdir}/lto-ltfs.conf",
        )


class RpmVerifierTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.tools = self.root / "tools"
        self.payload = self.root / "payload"
        self.tools.mkdir()
        self.payload.mkdir()
        self.rpm = self.root / "lto-ltfs-0.1.0-22.el9.x86_64.rpm"
        self.rpm.write_bytes(b"not-a-real-rpm-test-fixture")
        self.metadata = self.root / "metadata.txt"
        self.records = self.root / "records.txt"
        self.requires = self.root / "requires.txt"
        self.require_flags = self.root / "require-flags.txt"
        self.scripts = self.root / "scripts.txt"
        self._write_tools()
        self._write_valid_fixture()

    def _write_tool(self, name, body):
        path = self.tools / name
        path.write_text("#!/usr/bin/python3\n" + textwrap.dedent(body))
        path.chmod(0o755)

    def _write_tools(self):
        self._write_tool(
            "rpm",
            """
            import os
            import sys

            if os.environ.get("RPM_FIXTURE_OVERSIZED_QUERY"):
                block = b"x" * (1024 * 1024)
                for _ in range(17):
                    sys.stdout.buffer.write(block)
                raise SystemExit(0)

            arguments = sys.argv[1:]
            if "--scripts" in arguments:
                source = os.environ["RPM_FIXTURE_SCRIPTS"]
            elif any("REQUIREFLAGS" in argument for argument in arguments):
                source = os.environ["RPM_FIXTURE_REQUIRE_FLAGS"]
            elif "--requires" in arguments:
                source = os.environ["RPM_FIXTURE_REQUIRES"]
            elif any("FILENAMES" in argument for argument in arguments):
                source = os.environ["RPM_FIXTURE_RECORDS"]
            else:
                source = os.environ["RPM_FIXTURE_METADATA"]
            with open(source, "rb") as fixture:
                sys.stdout.buffer.write(fixture.read())
            """,
        )
        self._write_tool(
            "rpm2cpio",
            """
            import os
            import stat
            import sys
            from pathlib import Path

            output = sys.stdout.buffer
            inode = 1

            def pad(length):
                output.write(b"\\0" * ((-length) % 4))

            def entry(name, mode, data=b"", declared_size=None):
                global inode
                encoded_name = name.encode("utf-8") + b"\\0"
                size = len(data) if declared_size is None else declared_size
                fields = (
                    inode, mode, 0, 0, 1, 0, size,
                    0, 0, 0, 0, len(encoded_name), 0,
                )
                output.write(b"070701" + b"".join(
                    f"{field:08x}".encode("ascii") for field in fields
                ))
                output.write(encoded_name)
                pad(110 + len(encoded_name))
                output.write(data)
                pad(size)
                inode += 1

            marker = os.environ.get("RPM_FIXTURE_RPM2CPIO_MARKER")
            if marker:
                Path(marker).write_text("executed")

            attack = os.environ.get("RPM_FIXTURE_CPIO_ATTACK")
            if attack == "oversized":
                entry(
                    "./usr/bin/ltfs",
                    stat.S_IFREG | 0o755,
                    declared_size=64 * 1024 * 1024 + 1,
                )
            elif attack == "path":
                entry("../../rpm-verifier-escape", stat.S_IFREG | 0o644, b"escape")
            elif attack == "long-path":
                entry("./" + "x" * 4097, stat.S_IFREG | 0o644)
            elif attack == "special":
                entry("./usr/bin/ltfs", stat.S_IFIFO | 0o644)
            elif attack == "symlink-escape":
                entry(
                    "./usr/lib64/libltfs.so.0",
                    stat.S_IFLNK | 0o755,
                    b"../../../../rpm-verifier-symlink-escape",
                )
            else:
                source = Path(os.environ["RPM_FIXTURE_PAYLOAD"])
                records = Path(os.environ["RPM_FIXTURE_RECORDS"]).read_text().splitlines()
                included = {
                    line.rsplit("\t", 1)[-1].lstrip("/") for line in records if line
                }
                for item in source.rglob("*"):
                    payload_path = item.relative_to(source).as_posix()
                    if payload_path not in included:
                        continue
                    relative = "./" + payload_path
                    item_stat = item.lstat()
                    if item.is_symlink():
                        entry(relative, item_stat.st_mode, os.readlink(item).encode())
                    elif item.is_dir():
                        entry(relative, item_stat.st_mode)
                    else:
                        entry(relative, item_stat.st_mode, item.read_bytes())
            entry("TRAILER!!!", 0)
            """,
        )
        self._write_tool(
            "cpio",
            """
            import os
            import shutil
            import sys
            from pathlib import Path

            marker = os.environ.get("RPM_FIXTURE_EXTERNAL_CPIO_MARKER")
            if marker:
                Path(marker).write_text("executed")
            sys.stdin.buffer.read()
            source = Path(os.environ["RPM_FIXTURE_PAYLOAD"])
            for item in source.rglob("*"):
                relative = item.relative_to(source)
                target = Path.cwd() / relative
                if item.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                elif item.is_symlink():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.symlink_to(os.readlink(item))
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(item, target)
            """,
        )

    def _add_file(self, path, content, mode=0o644, group="root"):
        target = self.payload / path.lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(content, str):
            target.write_text(content, encoding="utf-8")
        else:
            target.write_bytes(content)
        target.chmod(mode)
        return f"{stat.S_IFREG | mode:o}\troot\t{group}\t{path}"

    def _add_symlink(self, path, target):
        link = self.payload / path.lstrip("/")
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(target)
        return f"{stat.S_IFLNK | 0o777:o}\troot\troot\t{path}"

    @staticmethod
    def _directory_record(path):
        return f"{stat.S_IFDIR | 0o755:o}\troot\troot\t{path}"

    def _write_valid_fixture(self):
        records = []
        for name in ("ltfs", "ltfsck", "ltfs-info"):
            content = b"\x7fELF\x00HPE\x00" if name == "ltfs-info" else "binary\n"
            records.append(self._add_file(f"/usr/bin/{name}", content, 0o755))
        records.append(
            self._add_file("/usr/bin/mkltfs", "binary\n", 0o750, "lto-admin")
        )
        records.extend(
            [
                self._directory_record("/usr/include/ltfs"),
                self._directory_record("/usr/lib64/ltfs"),
                self._directory_record("/usr/share/lto-ltfs/messages"),
                self._add_file(
                    "/usr/lib64/libltfs.so.0.0.0",
                    b"\x7fELF\x00" + b"2.4.8.4 (Prelim)\x00",
                ),
                self._add_symlink("/usr/lib64/libltfs.so.0", "libltfs.so.0.0.0"),
                self._add_symlink("/usr/lib64/libltfs.so", "libltfs.so.0.0.0"),
                self._add_file(
                    "/usr/lib64/ltfs/libtape-sg.so",
                    b"\x7fELF\x00HPE\x00HPE\x00HPE\x00"
                    b"HPE LTO - Cleaning Required\x00"
                    b"HPE LTO - Bad microcode detected\x00",
                ),
                self._add_file(
                    "/usr/lib64/pkgconfig/ltfs.pc",
                    "Name: ltfs\nDescription: LTFS\nVersion: 2.4.8.4 (Prelim)\n",
                ),
                self._add_file(
                    "/usr/include/ltfs/ltfs.h",
                    "/* SPDX-License-Identifier: BSD-3-Clause */\n",
                ),
                self._add_file(
                    "/usr/include/ltfs/config.h",
                    '#define PACKAGE_STRING "LTFS 2.4.8.4 (Prelim)"\n'
                    '#define PACKAGE_VERSION "2.4.8.4 (Prelim)"\n'
                    '#define VERSION "2.4.8.4 (Prelim)"\n',
                ),
                self._add_file(
                    "/usr/include/ltfs/libltfs/ltfs.h",
                    "\tNOLOCK_MAM    = 128,  /* From HPE */\n",
                ),
                self._add_file(
                    "/usr/include/ltfs/libltfs/ltfstrace.h",
                    "#define REQ_STATUS_MASK (0xF0000000)\n",
                ),
                self._add_file(
                    "/usr/include/ltfs/libltfs/tape_ops.h",
                    "#define TC_MAM_BARCODE_LEN TC_MAM_BARCODE_SIZE /* HPE LTFS alias */\n"
                    "\t * @param vol_name Volume name, unused by libtlfs (HPE extension)\n"
                    "\t * @param vol_name Volume barcode, unused by libtlfs (HPE extension)\n"
                    "\t * @param vol_mam_uuid Volume UUID, unused by libtlfs (HPE extension)\n",
                ),
                self._add_file(
                    "/usr/include/ltfs/tape_drivers/ibm_tape.h",
                    "\tVOLSTATS_USED_CAPACITY    = 0x0203,\t"
                    "/* HPE alias of VOLSTATS_PART_USED_CAP */\n",
                ),
                self._add_file("/usr/share/man/man8/ltfs-sde.8.gz", b"\x1f\x8b\x00"),
                self._add_file(
                    "/usr/share/lto-ltfs/messages/bin_ltfs/en.txt",
                    "safe message catalog\n",
                ),
                self._add_file(
                    "/usr/share/licenses/lto-ltfs/LICENSE", "BSD-3-Clause\n"
                ),
                self._add_file("/usr/share/licenses/lto-ltfs/NOTICES", "notices\n"),
                self._add_file(
                    "/usr/share/licenses/lto-ltfs/COPYING.LIB",
                    (ROOT / "COPYING.LIB").read_bytes(),
                ),
                self._add_file(
                    "/usr/share/licenses/lto-ltfs/LGPL-NOTICE",
                    (ROOT / "LGPL-NOTICE").read_bytes(),
                ),
                self._add_file(
                    "/usr/share/licenses/lto-ltfs/upstream.json",
                    "{\n"
                    '  "tag": "v2.4.8.4-10522",\n'
                    '  "commit": "7d0de7c0a71296353160f4c5bc082fec9af04e5c"\n'
                    "}\n",
                ),
                self._add_file(
                    "/usr/lib/udev/rules.d/99-lto-ltfs.rules",
                    'ACTION=="add", SUBSYSTEM=="scsi_generic", '
                    'KERNEL=="sg[0-9]*", ATTRS{type}=="1", '
                    'ENV{ID_SERIAL}=="", IMPORT{program}="/usr/lib/udev/scsi_id '
                    '--export --whitelisted --device=$devnode"\n'
                    'ACTION=="add", SUBSYSTEM=="scsi_generic", '
                    'KERNEL=="sg[0-9]*", ATTRS{type}=="1", '
                    'ENV{ID_SERIAL}!="", '
                    'SYMLINK+="lto-archiver-scsi-$env{ID_SERIAL}", '
                    'GROUP="lto-admin", MODE="0660", '
                    'OPTIONS+="string_escape=replace"\n',
                ),
                self._add_file(
                    "/usr/lib/tmpfiles.d/lto-ltfs.conf",
                    "d /run/lock/lto-ltfs 0770 root lto-admin -\n",
                ),
                self._add_file("/etc/ltfs.conf", "plugin tape sg safe\n", 0o644),
                self._add_file(
                    "/etc/ltfs.conf.local",
                    "# local overrides\n",
                    0o640,
                    "lto-admin",
                ),
                self._add_file(
                    "/etc/lto-ltfs/device.json",
                    '{"nst_path":"__UNPROVISIONED__",'
                    '"sg_path":"__UNPROVISIONED__",'
                    '"serial":"__UNPROVISIONED__",'
                    '"wwid":"__UNPROVISIONED__"}\n',
                    0o640,
                    "lto-admin",
                ),
            ]
        )
        self.records.write_text("\n".join(records) + "\n", encoding="utf-8")
        self.metadata.write_text(
            "lto-ltfs\t0.1.0\t22.el9\tx86_64\t"
            "BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only\t" + "a" * 64 + "\t0\n",
            encoding="utf-8",
        )
        self.requires.write_text(
            "/bin/sh\n"
            "/usr/bin/fusermount\n"
            "/usr/bin/systemd-tmpfiles\n"
            "/usr/lib/udev/scsi_id\n"
            "fuse-libs >= 2.9.9\n"
            "libxml2\n"
            "libuuid\n"
            "libicu\n"
            "openssl-libs\n"
            "shadow-utils\n"
            "zlib\n",
            encoding="utf-8",
        )
        self.require_flags.write_text(
            "/usr/bin/systemd-tmpfiles\t1024\t\n",
            encoding="utf-8",
        )
        self.scripts.write_text(
            "preinstall scriptlet (using /bin/sh):\n"
            "getent group lto-admin >/dev/null || /usr/sbin/groupadd -r lto-admin\n"
            "postinstall scriptlet (using /bin/sh):\n"
            "/usr/bin/systemd-tmpfiles --create "
            "/usr/lib/tmpfiles.d/lto-ltfs.conf\n",
            encoding="utf-8",
        )

    def run_verify(self, arguments=None, extra_environment=None, timeout=None):
        environment = os.environ.copy()
        environment.update(
            {
                "PATH": str(self.tools) + os.pathsep + environment["PATH"],
                "RPM_FIXTURE_METADATA": str(self.metadata),
                "RPM_FIXTURE_RECORDS": str(self.records),
                "RPM_FIXTURE_REQUIRES": str(self.requires),
                "RPM_FIXTURE_REQUIRE_FLAGS": str(self.require_flags),
                "RPM_FIXTURE_SCRIPTS": str(self.scripts),
                "RPM_FIXTURE_PAYLOAD": str(self.payload),
            }
        )
        environment.update(extra_environment or {})
        return subprocess.run(
            [sys.executable, str(VERIFY), *(arguments or [str(self.rpm)])],
            cwd=ROOT,
            env=environment,
            capture_output=True,
            text=True,
            check=False,
            timeout=timeout,
        )

    def _write_valid_source_rpm_fixture(self):
        source_rpm = self.root / "lto-ltfs-0.1.0-22.el9.src.rpm"
        source_rpm.write_bytes(b"not-a-real-source-rpm-test-fixture")
        source_payload = self.root / "source-payload"
        source_payload.mkdir()
        archive_bytes = subprocess.run(
            [
                "git",
                "archive",
                "--format=tar",
                "--prefix=lto-ltfs-0.1.0/",
                "HEAD",
            ],
            cwd=ROOT,
            check=True,
            capture_output=True,
        ).stdout
        with (
            (source_payload / "lto-ltfs-0.1.0.tar.gz").open("wb") as target,
            gzip.GzipFile(fileobj=target, mode="wb", mtime=0) as compressed,
        ):
            compressed.write(archive_bytes)
        for source, destination in (
            ("packaging/rpm/lto-ltfs.spec", "lto-ltfs.spec"),
            ("packaging/udev/99-lto-ltfs.rules", "99-lto-ltfs.rules"),
            ("packaging/tmpfiles/lto-ltfs.conf", "lto-ltfs.conf"),
        ):
            content = subprocess.run(
                ["git", "show", "HEAD:" + source],
                cwd=ROOT,
                check=True,
                capture_output=True,
            ).stdout
            (source_payload / destination).write_bytes(content)
        self.metadata.write_text(
            "lto-ltfs\t0.1.0\t22.el9\tx86_64\t"
            "BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only\t" + "b" * 64 + "\t1\n"
        )
        self.records.write_text(
            "lto-ltfs-0.1.0.tar.gz\nlto-ltfs.spec\n99-lto-ltfs.rules\nlto-ltfs.conf\n"
        )
        source_manifest = self.root / "SOURCE-MANIFEST.json"
        archive = source_payload / "lto-ltfs-0.1.0.tar.gz"
        source_manifest.write_text(
            json.dumps(
                {
                    "schema": 1,
                    "archive_name": archive.name,
                    "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                },
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
            encoding="utf-8",
        )
        return source_rpm, source_payload, source_manifest

    def test_realistic_rhel_payload_with_0777_library_symlinks_is_accepted(self):
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("RPM verification passed", result.stdout)

    def test_release_22_requires_complete_lgpl_notice_payload(self):
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)

        notice = self.payload / "usr/share/licenses/lto-ltfs/LGPL-NOTICE"
        notice.write_text("HPE attribution was replaced\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("notice", result.stderr.lower())
        notice.write_bytes((ROOT / "LGPL-NOTICE").read_bytes())

        self.records.write_text(
            "\n".join(
                row for row in self.records.read_text(encoding="utf-8").splitlines()
                if not row.endswith("/COPYING.LIB")
            ) + "\n",
            encoding="utf-8",
        )
        (self.payload / "usr/share/licenses/lto-ltfs/COPYING.LIB").unlink()
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("license", result.stderr.lower())

    def test_external_cpio_is_never_invoked(self):
        marker = self.root / "external-cpio-executed"
        result = self.run_verify(
            extra_environment={"RPM_FIXTURE_EXTERNAL_CPIO_MARKER": str(marker)}
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(marker.exists())

    def test_cpio_stream_limits_are_enforced_before_writing_an_entry(self):
        outside = self.root.parent / "rpm-verifier-escape"
        self.addCleanup(outside.unlink, missing_ok=True)
        for attack, expected in (
            ("oversized", "limit"),
            ("path", "path"),
            ("long-path", "path"),
            ("special", "type"),
        ):
            with self.subTest(attack=attack):
                result = self.run_verify(
                    extra_environment={"RPM_FIXTURE_CPIO_ATTACK": attack}
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr.lower())
                self.assertFalse(outside.exists())

    def test_cpio_symlink_cannot_escape_the_private_extraction_root(self):
        outside = self.root.parent / "rpm-verifier-symlink-escape"
        self.addCleanup(outside.unlink, missing_ok=True)
        result = self.run_verify(
            extra_environment={"RPM_FIXTURE_CPIO_ATTACK": "symlink-escape"}
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink escapes", result.stderr.lower())
        self.assertFalse(outside.exists())

    def test_library_symlinks_require_the_exact_relative_target(self):
        link = self.payload / "usr/lib64/libltfs.so"
        link.unlink()
        link.symlink_to("libltfs.so.0")

        result = self.run_verify()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("symlink target", result.stderr.lower())

    def test_library_symlink_metadata_is_exact(self):
        original = self.records.read_text(encoding="utf-8")
        for path in (
            "/usr/lib64/libltfs.so",
            "/usr/lib64/libltfs.so.0",
        ):
            before = f"120777\troot\troot\t{path}"
            for after in (
                f"120755\troot\troot\t{path}",
                f"120777\tdaemon\troot\t{path}",
                f"120777\troot\twheel\t{path}",
            ):
                with self.subTest(after=after):
                    self.records.write_text(
                        original.replace(before, after), encoding="utf-8"
                    )
                    result = self.run_verify()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("symlink metadata", result.stderr.lower())
        self.records.write_text(original, encoding="utf-8")

    def test_payload_entry_limit_is_checked_before_rpm2cpio_runs(self):
        original = self.records.read_text(encoding="utf-8")
        extra = "".join(
            f"40755\troot\troot\t/usr/share/lto-ltfs/messages/d{number}\n"
            for number in range(1025)
        )
        self.records.write_text(original + extra, encoding="utf-8")
        marker = self.root / "rpm2cpio-executed"
        result = self.run_verify(
            extra_environment={"RPM_FIXTURE_RPM2CPIO_MARKER": str(marker)}
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("entries", result.stderr.lower())
        self.assertFalse(marker.exists())

    def test_query_output_is_stopped_at_the_byte_limit(self):
        result = self.run_verify(extra_environment={"RPM_FIXTURE_OVERSIZED_QUERY": "1"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("query output exceeds", result.stderr.lower())

    def test_source_manifest_is_rejected_from_its_bounded_size_metadata(self):
        source_rpm, source_payload, source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        source_manifest.write_bytes(b"{" + b" " * 4096)
        old_payload = self.payload
        self.payload = source_payload
        result = self.run_verify(
            [
                "--srpm",
                str(source_rpm),
                "--source-manifest",
                str(source_manifest),
            ]
        )
        self.payload = old_payload
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("manifest exceeds", result.stderr.lower())

    def test_source_manifest_fifo_and_symlink_are_rejected_without_blocking(self):
        source_rpm, source_payload, source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        fifo = self.root / "SOURCE-MANIFEST.fifo"
        os.mkfifo(fifo)
        symlink = self.root / "SOURCE-MANIFEST.link"
        symlink.symlink_to(source_manifest)
        old_payload = self.payload
        self.payload = source_payload
        for candidate in (fifo, symlink):
            with self.subTest(candidate=candidate.name):
                try:
                    result = self.run_verify(
                        [
                            "--srpm",
                            str(source_rpm),
                            "--source-manifest",
                            str(candidate),
                        ],
                        timeout=2,
                    )
                except subprocess.TimeoutExpired:
                    self.fail("source manifest identity check blocked on a FIFO")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("manifest path is invalid", result.stderr.lower())
        self.payload = old_payload

    def test_forbidden_path_and_extra_executable_are_rejected(self):
        original = self.records.read_text(encoding="utf-8")
        for record, expected in (
            ("100644\troot\troot\t/root/private.txt\n", "forbidden path"),
            ("100755\troot\troot\t/usr/bin/java\n", "unexpected executable"),
            ("100644\troot\troot\t/usr/lib64/evil.so\n", "forbidden path"),
            ("100755\troot\troot\t/usr/share/ltfs/ltfs\n", "forbidden path"),
            (
                "100644\troot\troot\t/usr/share/snmp/LTFS-MIB.txt\n",
                "forbidden path",
            ),
            (
                "100644\troot\troot\t/usr/share/snmp/LtfsSnmpTrapDef.txt\n",
                "forbidden path",
            ),
        ):
            with self.subTest(record=record):
                self.records.write_text(original + record, encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr.lower())
        self.records.write_text(original, encoding="utf-8")

    def test_vendor_and_jre_named_libraries_are_outside_the_exact_allowlist(self):
        original = self.records.read_text(encoding="utf-8")
        for path in (
            "/usr/lib64/ltfs/libhpe-driver.so",
            "/usr/lib64/ltfs/libjre.so",
        ):
            with self.subTest(path=path):
                self.records.write_text(
                    original + f"100755\troot\troot\t{path}\n", encoding="utf-8"
                )
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("forbidden path", result.stderr.lower())
        self.records.write_text(original, encoding="utf-8")

    def test_hpe_is_rejected_in_every_payload_path_component(self):
        original = self.records.read_text(encoding="utf-8")
        for path in (
            "/usr/include/ltfs/HPE/safe.h",
            "/usr/include/ltfs/hpe/safe.h",
            "/usr/include/ltfs/prefixHPE/safe.h",
            "/usr/include/ltfs/HPEsuffix/safe.h",
            "/usr/include/ltfs/prefixHPEsuffix/safe.h",
        ):
            with self.subTest(path=path):
                record = self._add_file(path, b"safe header\n")
                self.records.write_text(original + record + "\n", encoding="utf-8")
                result = self.run_verify()
                (self.payload / path.lstrip("/")).unlink()
                self.records.write_text(original, encoding="utf-8")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("vendor or jre artifact", result.stderr.lower())

    def test_versioned_core_library_target_is_required_even_when_links_exist(self):
        library_path = "/usr/lib64/libltfs.so.0.0.0"
        retained = [
            line
            for line in self.records.read_text(encoding="utf-8").splitlines()
            if line.rsplit("\t", 1)[-1] != library_path
        ]
        self.records.write_text("\n".join(retained) + "\n", encoding="utf-8")
        (self.payload / library_path.lstrip("/")).unlink()
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("core library", result.stderr.lower())

    def test_versioned_core_library_target_is_a_regular_file(self):
        path = self.payload / "usr/lib64/libltfs.so.0.0.0"
        path.unlink()
        path.mkdir()
        self.records.write_text(
            self.records.read_text(encoding="utf-8").replace(
                "100644\troot\troot\t/usr/lib64/libltfs.so.0.0.0",
                "40755\troot\troot\t/usr/lib64/libltfs.so.0.0.0",
            ),
            encoding="utf-8",
        )

        result = self.run_verify()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("core library", result.stderr.lower())

    def test_versioned_core_library_target_is_root_owned(self):
        original = self.records.read_text(encoding="utf-8")
        before = "100644\troot\troot\t/usr/lib64/libltfs.so.0.0.0"
        for after in (
            "100644\tdaemon\troot\t/usr/lib64/libltfs.so.0.0.0",
            "100644\troot\twheel\t/usr/lib64/libltfs.so.0.0.0",
        ):
            with self.subTest(after=after):
                self.records.write_text(
                    original.replace(before, after), encoding="utf-8"
                )
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("core library", result.stderr.lower())
        self.records.write_text(original, encoding="utf-8")

    def test_unsafe_permissions_and_mkltfs_ownership_are_rejected(self):
        original = self.records.read_text(encoding="utf-8")
        mutations = (
            (
                "100755\troot\troot\t/usr/bin/ltfs",
                "104755\troot\troot\t/usr/bin/ltfs",
                "set-id",
            ),
            (
                "100644\troot\troot\t/etc/ltfs.conf",
                "100646\troot\troot\t/etc/ltfs.conf",
                "world-writable",
            ),
            (
                "100750\troot\tlto-admin\t/usr/bin/mkltfs",
                "100755\troot\troot\t/usr/bin/mkltfs",
                "mkltfs",
            ),
            (
                "100640\troot\tlto-admin\t/etc/lto-ltfs/device.json",
                "100644\troot\tlto-admin\t/etc/lto-ltfs/device.json",
                "device selector",
            ),
            (
                "100640\troot\tlto-admin\t/etc/lto-ltfs/device.json",
                "100640\troot\troot\t/etc/lto-ltfs/device.json",
                "device selector",
            ),
        )
        for before, after, expected in mutations:
            with self.subTest(after=after):
                self.records.write_text(
                    original.replace(before, after), encoding="utf-8"
                )
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr.lower())
        self.records.write_text(original, encoding="utf-8")

    def test_tape_touching_scriptlet_and_unsafe_dependency_are_rejected(self):
        self.scripts.write_text("postinstall:\nmkltfs --device=/dev/nst0\n")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("script", result.stderr.lower())
        self.scripts.write_text("preinstall:\ngroupadd -r lto-admin\n")
        self.requires.write_text(
            self.requires.read_text() + "libicuuc.so.50()(64bit)\n"
        )
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency", result.stderr.lower())

    def test_device_creation_and_udev_triggers_are_rejected_in_scriptlets(self):
        original = self.scripts.read_text(encoding="utf-8")
        for command in (
            "/usr/bin/udevadm trigger --subsystem-match=scsi_generic",
            "/usr/bin/udevadm settle",
            "/usr/bin/mknod /tmp/fake-device c 1 3",
            "/usr/bin/systemd-tmpfiles --create --prefix=/dev/lto-archiver",
        ):
            with self.subTest(command=command):
                self.scripts.write_text(original + command + "\n", encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("script", result.stderr.lower())
        self.scripts.write_text(original, encoding="utf-8")

    def test_tmpfiles_payload_rejects_device_namespace_creation(self):
        policy = self.payload / "usr/lib/tmpfiles.d/lto-ltfs.conf"
        original = policy.read_text(encoding="utf-8")
        policy.write_text(
            original + "d /dev/lto-archiver 0755 root root -\n",
            encoding="utf-8",
        )

        result = self.run_verify()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tmpfiles policy", result.stderr.lower())

    def test_udev_payload_rejects_the_legacy_nested_alias(self):
        policy = self.payload / "usr/lib/udev/rules.d/99-lto-ltfs.rules"
        policy.write_text(
            policy.read_text(encoding="utf-8").replace(
                "lto-archiver-scsi-$env{ID_SERIAL}",
                "lto-archiver/by-id/scsi-$env{ID_SERIAL}",
            ),
            encoding="utf-8",
        )

        result = self.run_verify()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("udev rule", result.stderr.lower())

    def test_scriptlets_and_dependencies_use_fail_closed_allowlists(self):
        original_scripts = self.scripts.read_text(encoding="utf-8")
        original_requires = self.requires.read_text(encoding="utf-8")
        self.scripts.write_text(original_scripts + "# harmless-looking mkltfs hook\n")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("script", result.stderr.lower())

        self.scripts.write_text(original_scripts)
        self.requires.write_text(original_requires + "java-17-openjdk\n")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency", result.stderr.lower())

    def test_tmpfiles_dependency_must_be_postinstall_qualified(self):
        self.require_flags.write_text(
            "/usr/bin/systemd-tmpfiles\t0\t\n",
            encoding="utf-8",
        )
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("postinstall dependency", result.stderr.lower())

    def test_build_sanitizer_does_not_become_a_driver_runtime_requirement(self):
        original = self.requires.read_text(encoding="utf-8")
        for dependency in ("libubsan", "libubsan.so.1()(64bit)"):
            with self.subTest(dependency=dependency):
                self.requires.write_text(original + dependency + "\n", encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("dependency", result.stderr.lower())

    def test_tmpfiles_dependency_rejects_additional_unqualified_requirement(self):
        self.require_flags.write_text(
            "/usr/bin/systemd-tmpfiles\t1024\t\n"
            "/usr/bin/systemd-tmpfiles\t0\t\n",
            encoding="utf-8",
        )
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("postinstall dependency", result.stderr.lower())

    def test_dependency_versions_release_and_binary_arch_are_exact(self):
        original_requires = self.requires.read_text(encoding="utf-8")
        original_metadata = self.metadata.read_text(encoding="utf-8")
        mutations = (
            original_requires.replace("libicu\n", "libicu < 0\n"),
            original_requires + "rpmlib(HPE-StoreOpen) <= 999\n",
            original_requires + "libc.so.6(HPE_1.0)(64bit)\n",
        )
        for dependency_set in mutations:
            with self.subTest(dependency_set=dependency_set):
                self.requires.write_text(dependency_set, encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("dependency", result.stderr.lower())
        self.requires.write_text(original_requires, encoding="utf-8")

        fields = original_metadata.rstrip("\n").split("\t")
        fields[2] = "10.evil"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release", result.stderr.lower())

        fields = original_metadata.rstrip("\n").split("\t")
        fields[3] = "aarch64"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("architecture", result.stderr.lower())

    def test_signed_sg_endian_release_21_is_not_the_lgpl_candidate(self):
        fields = self.metadata.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[2] = "21.el9"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release", result.stderr.lower())

    def test_release_20_is_not_the_sg_endian_candidate(self):
        fields = self.metadata.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[2] = "20.el9"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release", result.stderr.lower())

    def test_release_19_is_not_the_mam_volume_identifier_candidate(self):
        fields = self.metadata.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[2] = "19.el9"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release", result.stderr.lower())

    def test_release_18_is_not_the_read_only_session_candidate(self):
        fields = self.metadata.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[2] = "18.el9"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release", result.stderr.lower())

    def test_release_17_is_not_the_read_only_capacity_candidate(self):
        fields = self.metadata.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[2] = "17.el9"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release", result.stderr.lower())

    def test_release_18_nevra_is_required_for_read_only_capacity_diagnostics(self):
        source = LTFS_INFO_SOURCE.read_text(encoding="utf-8")
        self.assertIn('strcmp(mode, "pre-format")', source)
        mount_source = (ROOT / "src" / "main.c").read_text(encoding="utf-8")
        self.assertIn('"-ofsname=ltfs"', mount_source)
        self.assertNotIn('"-ofsname=ltfs:%s"', mount_source)
        self.assertEqual(self.run_verify().returncode, 0)
        original = self.metadata.read_text(encoding="utf-8")
        for release in (
            "17.el9",
            "16.el9",
            "15.el9",
            "14.el9",
            "13.el9",
            "12.el9",
            "11.el9",
            "10.el9",
            "9.el9",
            "8.el9",
            "7.el9",
            "6.el9",
            "5.el9",
            "4.el9",
            "3.el9",
            "2.el9",
            "1.el9",
        ):
            with self.subTest(release=release):
                fields = original.rstrip("\n").split("\t")
                fields[2] = release
                self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("release", result.stderr.lower())
        self.metadata.write_text(original, encoding="utf-8")

    def test_exact_real_rhel9_auto_requires_are_accepted(self):
        self.requires.write_text(REAL_RHEL9_REQUIRES, encoding="utf-8")
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fuse2_mount_helper_dependency_is_exact(self):
        original_requires = self.requires.read_text(encoding="utf-8")
        self.assertIn("/usr/bin/fusermount\n", original_requires)
        mutations = (
            original_requires.replace("/usr/bin/fusermount\n", ""),
            original_requires.replace(
                "/usr/bin/fusermount\n", "/usr/bin/fusermount3\n"
            ),
            original_requires + "/usr/bin/fusermount3\n",
        )
        for requires in mutations:
            with self.subTest(requires=requires):
                self.requires.write_text(requires, encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("dependency", result.stderr.lower())
        self.requires.write_text(original_requires, encoding="utf-8")

    def test_udev_scsi_id_dependency_is_exact(self):
        original_requires = self.requires.read_text(encoding="utf-8")
        self.assertIn("/usr/lib/udev/scsi_id\n", original_requires)
        for requires in (
            original_requires.replace("/usr/lib/udev/scsi_id\n", ""),
            original_requires.replace(
                "/usr/lib/udev/scsi_id\n", "/usr/bin/scsi_id\n"
            ),
        ):
            with self.subTest(requires=requires):
                self.requires.write_text(requires, encoding="utf-8")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("dependency", result.stderr.lower())
        self.requires.write_text(original_requires, encoding="utf-8")

    def test_real_rhel9_dependency_allowlist_rejects_path_mutations(self):
        for replacement in (
            "/usr/bin/pkg-config >= 999",
            "/usr/bin/pkgconf",
            "/usr/bin/curl",
        ):
            with self.subTest(replacement=replacement):
                self.requires.write_text(
                    REAL_RHEL9_REQUIRES.replace("/usr/bin/pkg-config", replacement),
                    encoding="utf-8",
                )
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("dependency", result.stderr.lower())

    def test_secret_private_ip_and_configured_serial_are_rejected(self):
        config = self.payload / "etc/lto-ltfs/device.json"
        original = config.read_text()
        mutations = (
            (
                (
                    '{"nst_path":"/dev/tape/by-id/test-nst",'
                    '"sg_path":"/dev/lto-archiver-scsi-test-sg",'
                    '"serial":"ZZ12345678","wwid":"naa.test"}\n'
                ),
                "serial",
            ),
            (
                (
                    '{"nst_path":"__UNPROVISIONED__",'
                    '"sg_path":"__UNPROVISIONED__",'
                    '"serial":"__UNPROVISIONED__",'
                    '"wwid":"__UNPROVISIONED__","password":"secret"}\n'
                ),
                "secret",
            ),
            (
                (
                    '{"nst_path":"__UNPROVISIONED__",'
                    '"sg_path":"__UNPROVISIONED__",'
                    '"serial":"__UNPROVISIONED__",'
                    '"wwid":"__UNPROVISIONED__","endpoint":"10.2.3.4"}\n'
                ),
                "ip address",
            ),
            (
                '{"schema":1,"expected_serial":null,"expected_wwid":null}\n',
                "schema",
            ),
        )
        for content, expected in mutations:
            with self.subTest(content=content):
                config.write_text(content)
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stderr.lower())
        config.write_text(original)

    def test_product_version_mutation_is_rejected_in_text_and_binary_payloads(self):
        pkgconfig = self.payload / "usr/lib64/pkgconfig/ltfs.pc"
        library = self.payload / "usr/lib64/libltfs.so.0.0.0"
        provenance = self.payload / "usr/share/licenses/lto-ltfs/upstream.json"
        original_pkgconfig = pkgconfig.read_bytes()
        original_library = library.read_bytes()
        original_provenance = provenance.read_bytes()
        for path, original in (
            (pkgconfig, original_pkgconfig),
            (library, original_library),
            (provenance, original_provenance),
        ):
            with self.subTest(path=path.name):
                path.write_bytes(original.replace(b"2.4.8.4", b"2.4.8.5"))
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ip address", result.stderr.lower())
                path.write_bytes(original)

    def test_canonical_product_version_is_limited_to_exact_binary_paths(self):
        original_records = self.records.read_text(encoding="utf-8")
        version_field = b"\x7fELF\x00" + b"2.4.8.4 (Prelim)\x00"
        for path in (
            "/usr/lib64/ltfs/libunexpected.so",
            "/usr/lib64/ltfs/nested/libunexpected.so",
        ):
            with self.subTest(path=path):
                record = self._add_file(path, version_field)
                self.records.write_text(
                    original_records + record + "\n", encoding="utf-8"
                )
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("forbidden path", result.stderr.lower())
                (self.payload / path.lstrip("/")).unlink()
        self.records.write_text(original_records, encoding="utf-8")

    def test_canonical_product_version_is_accepted_in_frozen_binary_paths(self):
        exact_binary_paths = (
            "/usr/bin/ltfs",
            "/usr/bin/ltfs-info",
            "/usr/bin/ltfsck",
            "/usr/bin/mkltfs",
            "/usr/lib64/libltfs.so.0.0.0",
            "/usr/lib64/ltfs/libiosched-fcfs.so",
            "/usr/lib64/ltfs/libiosched-unified.so",
            "/usr/lib64/ltfs/libkmi-flatfile.so",
            "/usr/lib64/ltfs/libkmi-simple.so",
            "/usr/lib64/ltfs/libtape-file.so",
            "/usr/lib64/ltfs/libtape-itdtimg.so",
            "/usr/lib64/ltfs/libtape-sg.so",
        )
        original_records = self.records.read_text(encoding="utf-8")
        version_field = b"\x7fELF\x00" + b"2.4.8.4 (Prelim)\x00"
        for path in exact_binary_paths:
            with self.subTest(path=path):
                target = self.payload / path.lstrip("/")
                original_payload = target.read_bytes() if target.exists() else None
                if original_payload is None:
                    record = self._add_file(path, version_field)
                    self.records.write_text(
                        original_records + record + "\n", encoding="utf-8"
                    )
                else:
                    target.write_bytes(
                        version_field + original_payload
                        if path in ("/usr/lib64/ltfs/libtape-sg.so", "/usr/bin/ltfs-info")
                        else version_field
                    )
                    self.records.write_text(original_records, encoding="utf-8")
                result = self.run_verify()
                self.assertEqual(result.returncode, 0, result.stderr)
                if original_payload is None:
                    target.unlink()
                else:
                    target.write_bytes(original_payload)
        self.records.write_text(original_records, encoding="utf-8")

    def test_canonical_product_version_requires_exact_text_context(self):
        cases = (
            (
                self.payload / "usr/lib64/pkgconfig/ltfs.pc",
                b"Version: 2.4.8.4 (Prelim)",
                b"Endpoint: 2.4.8.4 (Prelim)",
            ),
            (
                self.payload / "usr/share/licenses/lto-ltfs/upstream.json",
                b'"tag": "v2.4.8.4-10522"',
                b'"endpoint": "v2.4.8.4-10522"',
            ),
            (
                self.payload / "usr/share/lto-ltfs/messages/bin_ltfs/en.txt",
                b"safe message catalog",
                b"Version: 2.4.8.4 (Prelim)",
            ),
        )
        for path, before, after in cases:
            with self.subTest(path=path):
                original = path.read_bytes()
                self.assertIn(before, original)
                path.write_bytes(original.replace(before, after))
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ip address", result.stderr.lower())
                path.write_bytes(original)

    def test_canonical_binary_version_requires_both_nul_boundaries(self):
        library = self.payload / "usr/lib64/libltfs.so.0.0.0"
        original = library.read_bytes()
        for payload in (
            b"2.4.8.4 (Prelim)\x00",
            b"\x00" + b"2.4.8.4 (Prelim)",
            b"\x00endpoint=2.4.8.4 (Prelim)\x00",
        ):
            with self.subTest(payload=payload):
                library.write_bytes(payload)
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ip address", result.stderr.lower())
        library.write_bytes(original)

    def test_core_version_x86_64_immediate_requires_exact_path_and_opcodes(self):
        core = self.payload / "usr/lib64/libltfs.so.0.0.0"
        executable = self.payload / "usr/bin/ltfs"
        original_core = core.read_bytes()
        original_executable = executable.read_bytes()
        exact = b"\x7fELF\x48\xbe2.4.8.4 \x48\x89\x30"
        optimized = b"\x7fELF\x48\xb82.4.8.4 \x48\x89\x43"

        for payload in (exact, optimized):
            with self.subTest(payload=payload):
                core.write_bytes(payload)
                result = self.run_verify()
                self.assertEqual(result.returncode, 0, result.stderr)

        mutations = (
            (core, exact.replace(b"\x48\xbe", b"\x49\xbe")),
            (core, exact.replace(b"2.4.8.4 ", b"2.4.8.5 ")),
            (core, exact.replace(b"2.4.8.4 ", b"2.4.8.4X")),
            (core, exact.replace(b"\x48\x89\x30", b"\x48\x89\x31")),
            (core, optimized.replace(b"\x48\x89\x43", b"\x48\x89\x42")),
            (executable, exact),
            (core, exact + b" endpoint=10.2.3.4"),
        )
        for target, payload in mutations:
            with self.subTest(target=target, payload=payload):
                core.write_bytes(original_core)
                executable.write_bytes(original_executable)
                target.write_bytes(payload)
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ip address", result.stderr.lower())
        core.write_bytes(original_core)
        executable.write_bytes(original_executable)

    def test_installed_config_header_version_requires_exact_path_and_lines(self):
        header = self.payload / "usr/include/ltfs/config.h"
        original = header.read_bytes()
        original_records = self.records.read_text(encoding="utf-8")
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)

        mutations = (
            (
                "/usr/include/ltfs/config.h",
                original.replace(b"PACKAGE_VERSION", b"PACKAGE_VERSIONS", 1),
            ),
            ("/usr/include/ltfs/config.hpp", original),
            (
                "/usr/include/ltfs/config.h",
                original.replace(b"2.4.8.4", b"2.4.8.5", 1),
            ),
            (
                "/usr/include/ltfs/config.h",
                original.replace(
                    b'#define PACKAGE_STRING "LTFS 2.4.8.4 (Prelim)"',
                    b'#define PACKAGE_STRING "LTFS 2.4.8.4 (Prelim)" suffix',
                ),
            ),
            (
                "/usr/include/ltfs/config.h",
                original.replace(
                    b'#define VERSION "2.4.8.4 (Prelim)"',
                    b'prefix #define VERSION "2.4.8.4 (Prelim)"',
                ),
            ),
            (
                "/usr/include/ltfs/config.h",
                original + b'#define PRIVATE "10.2.3.4"\n',
            ),
            (
                "/usr/include/ltfs/config.h",
                original + b'#define PUBLIC "8.8.8.8"\n',
            ),
        )
        for path, payload in mutations:
            with self.subTest(path=path, payload=payload):
                target = self.payload / path.lstrip("/")
                target.write_bytes(payload)
                if target != header:
                    self.records.write_text(
                        original_records.replace(
                            "/usr/include/ltfs/config.h", path
                        ),
                        encoding="utf-8",
                    )
                result = self.run_verify()
                if target != header:
                    target.unlink()
                header.write_bytes(original)
                self.records.write_text(original_records, encoding="utf-8")
                self.assertNotEqual(result.returncode, 0)

    def test_installed_config_header_is_required_regular_and_root_owned(self):
        header_path = "/usr/include/ltfs/config.h"
        header = self.payload / header_path.lstrip("/")
        original_header = header.read_bytes()
        original_records = self.records.read_text(encoding="utf-8")
        record = next(
            line for line in original_records.splitlines() if line.endswith(header_path)
        )

        header.unlink()
        self.records.write_text(
            original_records.replace(record + "\n", ""), encoding="utf-8"
        )
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)

        metadata_mutations = (
            record.replace("\troot\troot\t", "\tnobody\troot\t"),
            record.replace("\troot\troot\t", "\troot\twheel\t"),
            record.replace("100644\t", "100646\t"),
            record.replace("100644\t", "120777\t"),
        )
        for mutated_record in metadata_mutations:
            with self.subTest(mutated_record=mutated_record):
                self._add_file(header_path, original_header)
                self.records.write_text(
                    original_records.replace(record, mutated_record), encoding="utf-8"
                )
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)

        header.unlink()
        header.symlink_to("ltfs.h")
        self.records.write_text(original_records, encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)

    def test_installed_config_header_requires_each_canonical_macro_once(self):
        header = self.payload / "usr/include/ltfs/config.h"
        original = header.read_bytes()
        lines = original.splitlines(keepends=True)

        header.write_bytes(original + b"#define HAVE_SAFE_GENERATED_MACRO 1\n")
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)

        mutations = []
        for line in lines:
            mutations.extend(
                (
                    original.replace(line, b"", 1),
                    original + line,
                    line,
                )
            )
        mutations.append(original + original)
        for payload in mutations:
            with self.subTest(payload=payload):
                header.write_bytes(payload)
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)

    def test_hpe_header_compatibility_requires_exact_paths_lines_and_counts(self):
        contracts = {
            "/usr/include/ltfs/libltfs/ltfs.h": (
                b"\tNOLOCK_MAM    = 128,  /* From HPE */\n",
            ),
            "/usr/include/ltfs/libltfs/tape_ops.h": (
                b"#define TC_MAM_BARCODE_LEN TC_MAM_BARCODE_SIZE /* HPE LTFS alias */\n",
                b"\t * @param vol_name Volume name, unused by libtlfs (HPE extension)\n",
                b"\t * @param vol_name Volume barcode, unused by libtlfs (HPE extension)\n",
                b"\t * @param vol_mam_uuid Volume UUID, unused by libtlfs (HPE extension)\n",
            ),
            "/usr/include/ltfs/tape_drivers/ibm_tape.h": (
                (
                    b"\tVOLSTATS_USED_CAPACITY    = 0x0203,\t"
                    b"/* HPE alias of VOLSTATS_PART_USED_CAP */\n"
                ),
            ),
        }
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)

        for path, expected_lines in contracts.items():
            header = self.payload / path.lstrip("/")
            original = header.read_bytes()
            self.assertEqual(original, b"".join(expected_lines))
            mutations = []
            for line in expected_lines:
                mutations.extend(
                    (
                        original.replace(line, b"", 1),
                        original + line,
                        original.replace(line, b"prefix " + line, 1),
                        original.replace(line, line.rstrip(b"\n") + b" suffix\n", 1),
                        original.replace(line, line.replace(b"HPE", b"HPX"), 1),
                        original.replace(line, line.replace(b"HPE", b"hpe"), 1),
                    )
                )
            for payload in mutations:
                with self.subTest(path=path, payload=payload):
                    header.write_bytes(payload)
                    result = self.run_verify()
                    header.write_bytes(original)
                    self.assertNotEqual(result.returncode, 0)

        other = self.payload / "usr/include/ltfs/ltfs.h"
        original_other = other.read_bytes()
        other.write_bytes(original_other + contracts[next(iter(contracts))][0])
        result = self.run_verify()
        other.write_bytes(original_other)
        self.assertNotEqual(result.returncode, 0)

        other.write_bytes(original_other + b"prefixHPEsuffix\n")
        result = self.run_verify()
        other.write_bytes(original_other)
        self.assertNotEqual(result.returncode, 0)

        original_records = self.records.read_text(encoding="utf-8")
        for path in contracts:
            with self.subTest(missing_path=path):
                header = self.payload / path.lstrip("/")
                original = header.read_bytes()
                record = next(
                    line
                    for line in original_records.splitlines()
                    if line.endswith("\t" + path)
                )
                header.unlink()
                self.records.write_text(
                    original_records.replace(record + "\n", ""), encoding="utf-8"
                )
                result = self.run_verify()
                self._add_file(path, original)
                self.records.write_text(original_records, encoding="utf-8")
                self.assertNotEqual(result.returncode, 0)

    def test_sg_hpe_fields_require_exact_nul_boundaries_paths_and_counts(self):
        sg = self.payload / "usr/lib64/ltfs/libtape-sg.so"
        original = sg.read_bytes()
        expected_fields = (
            b"HPE",
            b"HPE",
            b"HPE",
            b"HPE LTO - Cleaning Required",
            b"HPE LTO - Bad microcode detected",
        )
        fields = original.split(b"\x00")
        for expected in set(expected_fields):
            self.assertEqual(fields.count(expected), expected_fields.count(expected))
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)

        optimized = (
            b"\x7fELF\x00HPE\x00HPE\x00HPE LTO - Cleaning Required\x00"
            b"HPE LTO - Bad microcode detected\x00"
        )
        sg.write_bytes(optimized)
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        for mutation in (
            optimized + b"HPE\x00HPE\x00",
            optimized.replace(b"HPE\x00", b"", 1),
            optimized.replace(b"HPE LTO - Cleaning Required\x00", b"", 1),
        ):
            sg.write_bytes(mutation)
            result = self.run_verify()
            self.assertNotEqual(result.returncode, 0)
        sg.write_bytes(original)

        mutations = []
        for field in dict.fromkeys(expected_fields):
            delimited = field + b"\x00"
            mutations.extend(
                (
                    original + delimited,
                    original.replace(delimited, b"prefix " + delimited, 1),
                    original.replace(delimited, field + b" suffix\x00", 1),
                    original.replace(
                        delimited, field.replace(b"HPE", b"hpe") + b"\x00", 1
                    ),
                )
            )
            if field != b"HPE":
                mutations.extend(
                    (
                        original.replace(delimited, b"", 1),
                        original.replace(
                            delimited,
                            field.replace(b"HPE", b"HPX") + b"\x00",
                            1,
                        ),
                    )
                )
        mutations.extend((original + b"StoreOpen\x00", original + b"jre\x00"))
        mutations.extend(
            (
                original.rstrip(b"\x00"),
                b"HPE\x00" + original.replace(b"\x00HPE\x00", b"\x00", 1),
            )
        )
        for payload in mutations:
            with self.subTest(payload=payload):
                sg.write_bytes(payload)
                result = self.run_verify()
                sg.write_bytes(original)
                self.assertNotEqual(result.returncode, 0)

        original_records = self.records.read_text(encoding="utf-8")
        foreign_path = "/usr/lib64/ltfs/libtape-file.so"
        foreign = self.payload / foreign_path.lstrip("/")
        record = self._add_file(foreign_path, original)
        self.records.write_text(original_records + record + "\n", encoding="utf-8")
        result = self.run_verify()
        foreign.unlink()
        self.records.write_text(original_records, encoding="utf-8")
        self.assertNotEqual(result.returncode, 0)

        record = next(
            line
            for line in original_records.splitlines()
            if line.endswith("\t/usr/lib64/ltfs/libtape-sg.so")
        )
        sg.unlink()
        self.records.write_text(
            original_records.replace(record + "\n", ""), encoding="utf-8"
        )
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)

    def test_info_vendor_field_requires_exact_path_boundaries_and_count(self):
        info = self.payload / "usr/bin/ltfs-info"
        original = info.read_bytes()
        valid = b"\x7fELF\x00HPE\x00"
        info.write_bytes(valid)
        result = self.run_verify()
        self.assertEqual(result.returncode, 0, result.stderr)
        for payload in (
            b"\x7fELF\x00", valid + b"HPE\x00", b"\x7fELF\x00prefixHPE\x00",
            b"\x7fELF\x00HPEsuffix\x00", b"\x7fELF\x00hpe\x00",
            b"HPE\x00", b"\x7fELF\x00HPE", valid + b"StoreOpen\x00",
            valid + b"jre\x00", valid + b"BEGIN PRIVATE KEY\x00",
        ):
            with self.subTest(payload=payload):
                info.write_bytes(payload)
                self.assertNotEqual(self.run_verify().returncode, 0)
        info.write_bytes(original)
        other = self.payload / "usr/bin/ltfsck"
        other.write_bytes(valid)
        self.assertNotEqual(self.run_verify().returncode, 0)

    def test_documentation_network_allowlist_is_preserved(self):
        config = self.payload / "etc/ltfs.conf"
        original = config.read_bytes()
        for address in (b"192.0.2.1", b"198.51.100.9", b"203.0.113.10"):
            with self.subTest(address=address):
                config.write_bytes(original + b"endpoint=" + address + b"\n")
                result = self.run_verify()
                self.assertEqual(result.returncode, 0, result.stderr)
        config.write_bytes(original)

    def test_private_public_and_version_shaped_ips_remain_rejected(self):
        library = self.payload / "usr/lib64/libltfs.so.0.0.0"
        original = library.read_bytes()
        for candidate in (b"10.2.3.4", b"8.8.8.8", b"2.4.8.4"):
            with self.subTest(candidate=candidate):
                library.write_bytes(original + b"endpoint=" + candidate + b"\x00")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ip address", result.stderr.lower())
        library.write_bytes(original)

    def test_serial_in_message_and_forbidden_binary_content_are_rejected(self):
        message = self.payload / "usr/share/lto-ltfs/messages/bin_ltfs/en.txt"
        original_message = message.read_bytes()
        message.write_text("diagnostic serial ZZ123456789\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("serial", result.stderr.lower())

        message.write_bytes(original_message)
        library = self.payload / "usr/lib64/libltfs.so.0"
        original_library = library.read_bytes()
        for marker in (b"HPE", b"StoreOpen", b"BEGIN PRIVATE KEY"):
            with self.subTest(marker=marker):
                library.write_bytes(b"\x7fELF\x00" + marker + b"\x00")
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                expected = "secret" if b"PRIVATE KEY" in marker else "vendor"
                self.assertIn(expected, result.stderr.lower())
        library.write_bytes(original_library)

    def test_trace_status_mask_is_the_only_serial_shaped_header_exception(self):
        header = self.payload / "usr/include/ltfs/libltfs/ltfstrace.h"
        original = header.read_bytes()
        for content in (
            b"#define REQ_STATUS_MASK (0xF0000001)\n",
            b"prefix #define REQ_STATUS_MASK (0xF0000000)\n",
            original + original,
            original + b"diagnostic serial ZZ123456789\n",
        ):
            with self.subTest(content=content):
                header.write_bytes(content)
                result = self.run_verify()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("serial", result.stderr.lower())
        header.write_bytes(original)

        records = self.records.read_text(encoding="utf-8")
        foreign_path = "/usr/include/ltfs/libltfs/nested/ltfstrace.h"
        record = self._add_file(foreign_path, original)
        self.records.write_text(records + record + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("serial", result.stderr.lower())

    def test_source_rpm_is_provenance_verified_and_tampering_is_rejected(self):
        source_rpm, source_payload, source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        old_payload = self.payload
        self.payload = source_payload
        result = self.run_verify(
            [
                "--srpm",
                str(source_rpm),
                "--source-manifest",
                str(source_manifest),
            ]
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SRPM verification passed", result.stdout)

        (source_payload / "99-lto-ltfs.rules").write_text("tampered\n")
        result = self.run_verify(
            [
                "--srpm",
                str(source_rpm),
                "--source-manifest",
                str(source_manifest),
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source", result.stderr.lower())
        self.payload = old_payload

    def test_source_package_header_marker_cannot_be_swapped(self):
        original_metadata = self.metadata.read_text(encoding="utf-8")
        fields = original_metadata.rstrip("\n").split("\t")
        fields[6] = "1"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        result = self.run_verify()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source/binary identity", result.stderr.lower())

        source_rpm, source_payload, source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        fields = self.metadata.read_text(encoding="utf-8").rstrip("\n").split("\t")
        fields[6] = "0"
        self.metadata.write_text("\t".join(fields) + "\n", encoding="utf-8")
        old_payload = self.payload
        self.payload = source_payload
        result = self.run_verify(
            [
                "--srpm",
                str(source_rpm),
                "--source-manifest",
                str(source_manifest),
            ]
        )
        self.payload = old_payload
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source/binary identity", result.stderr.lower())

    def test_source_rpm_rejects_coherent_spec_tamper_against_trusted_manifest(self):
        source_rpm, source_payload, source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        unpacked = self.root / "unpacked"
        unpacked.mkdir()
        archive = source_payload / "lto-ltfs-0.1.0.tar.gz"
        with tarfile.open(archive, "r:gz") as source:
            source.extractall(unpacked)
        spec = unpacked / "lto-ltfs-0.1.0/packaging/rpm/lto-ltfs.spec"
        tampered = spec.read_text() + "\n%post\n/usr/bin/mkltfs -d $DEVICE\n"
        spec.write_text(tampered)
        (source_payload / "lto-ltfs.spec").write_text(tampered)
        with tarfile.open(archive, "w:gz") as target:
            target.add(unpacked / "lto-ltfs-0.1.0", arcname="lto-ltfs-0.1.0")

        old_payload = self.payload
        self.payload = source_payload
        result = self.run_verify(
            [
                "--srpm",
                str(source_rpm),
                "--source-manifest",
                str(source_manifest),
            ]
        )
        self.payload = old_payload
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("manifest", result.stderr.lower())

    def test_source_rpm_never_executes_its_contained_provenance_checker(self):
        source_rpm, source_payload, source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        unpacked = self.root / "unpacked-checker"
        unpacked.mkdir()
        archive = source_payload / "lto-ltfs-0.1.0.tar.gz"
        with tarfile.open(archive, "r:gz") as source:
            source.extractall(unpacked)
        marker = self.root / "untrusted-checker-executed"
        checker = unpacked / "lto-ltfs-0.1.0/scripts/check-provenance.py"
        checker.write_text(
            "#!/usr/bin/env python3\n"
            "from pathlib import Path\n"
            f"Path({str(marker)!r}).write_text('executed')\n"
        )
        with tarfile.open(archive, "w:gz") as target:
            target.add(unpacked / "lto-ltfs-0.1.0", arcname="lto-ltfs-0.1.0")

        old_payload = self.payload
        self.payload = source_payload
        result = self.run_verify(
            [
                "--srpm",
                str(source_rpm),
                "--source-manifest",
                str(source_manifest),
            ]
        )
        self.payload = old_payload
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(marker.exists())

    def test_source_rpm_requires_an_external_manifest(self):
        source_rpm, source_payload, _source_manifest = (
            self._write_valid_source_rpm_fixture()
        )
        old_payload = self.payload
        self.payload = source_payload
        result = self.run_verify(["--srpm", str(source_rpm)])
        self.payload = old_payload
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source-manifest", result.stderr.lower())


class RpmBuildScriptTests(unittest.TestCase):
    def test_runtime_uninstall_checks_exact_binary_paths_without_shell_cache(self):
        containerfile = RPM_CONTAINERFILE.read_text(encoding="utf-8")
        executables = ("ltfs", "ltfsck", "mkltfs", "ltfs-info")

        def assert_exact_uninstall_checks(candidate):
            uninstall_tail = candidate.split("&& rpm -e lto-ltfs \\\n", 1)[1]
            self.assertNotIn("command -v", uninstall_tail)
            uninstall_lines = [
                line.strip().removesuffix("\\").strip()
                for line in uninstall_tail.splitlines()
            ]
            for executable in executables:
                check = f"&& test ! -e /usr/bin/{executable}"
                self.assertEqual(uninstall_lines.count(check), 1)

        assert_exact_uninstall_checks(containerfile)
        for executable in executables:
            with self.subTest(missing_check=executable):
                check = f"&& test ! -e /usr/bin/{executable}"
                mutated = containerfile.replace(check, "&& true", 1)
                with self.assertRaises(AssertionError):
                    assert_exact_uninstall_checks(mutated)

    def test_full_build_is_pinned_two_pass_container_verification(self):
        containerfile = RPM_CONTAINERFILE.read_text(encoding="utf-8")
        self.assertRegex(
            containerfile,
            r"FROM registry\.access\.redhat\.com/ubi9/ubi@sha256:[0-9a-f]{64}",
        )
        for required in (
            "rpmbuild -ba",
            "verify-rpm.py",
            "--srpm",
            "rpm -Uvh",
            "rpm -V lto-ltfs",
            "rpm -e lto-ltfs",
            "ltfs-info --self-test-fixture ready",
            "test -x /usr/bin/fusermount",
        ):
            self.assertIn(required, containerfile)
        self.assertNotIn("fusermount3", containerfile)
        self.assertNotIn("dnf install", containerfile)
        runtime_stage = containerfile.split(" AS runtime-test\n", 1)[1]
        self.assertNotIn("rpmbuild", runtime_stage)
        self.assertNotIn("/workspace/BUILD/", runtime_stage)

        build = BUILD.read_text(encoding="utf-8")
        self.assertIn("podman", build)
        self.assertIn("for build_number in 1 2", build)
        self.assertIn("cmp --silent", build)
        self.assertNotIn("command -v rpmbuild", build)

    def test_container_driver_runs_two_builds_and_rejects_a_byte_mismatch(self):
        for mismatch in (False, True):
            with self.subTest(mismatch=mismatch):
                temporary = tempfile.TemporaryDirectory()
                self.addCleanup(temporary.cleanup)
                fixture_root = Path(temporary.name)
                checkout = fixture_root / "checkout"
                tools = fixture_root / "tools"
                checkout.mkdir()
                tools.mkdir()
                for source in (
                    BUILD.relative_to(ROOT),
                    VERIFY.relative_to(ROOT),
                    RPM_CONTAINERFILE.relative_to(ROOT),
                    SPEC.relative_to(ROOT),
                    UDEV.relative_to(ROOT),
                    TMPFILES.relative_to(ROOT),
                ):
                    destination = checkout / source
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(ROOT / source, destination)
                (checkout / "payload.txt").write_text("tracked\n")
                bundle = fixture_root / "rpm-bundle"
                (bundle / "build").mkdir(parents=True)
                (bundle / "runtime").mkdir()
                bundle_files = {
                    "build/build-dependency.rpm": b"build dependency\n",
                    "runtime/runtime-dependency.rpm": b"runtime dependency\n",
                }
                for relative, content in bundle_files.items():
                    (bundle / relative).write_bytes(content)
                bundle_lock = fixture_root / "RPM-BUNDLE.sha256"
                bundle_lock.write_text(
                    "\n".join(
                        hashlib.sha256(content).hexdigest() + "  " + relative
                        for relative, content in sorted(bundle_files.items())
                    )
                    + "\n"
                )
                fake_podman = tools / "podman"
                fake_podman.write_text(
                    "#!/usr/bin/python3\n"
                    + textwrap.dedent(
                        """
                        import hashlib
                        import os
                        import sys
                        from pathlib import Path

                        required = {"--no-cache", "--pull=never", "--network=none"}
                        if not required.issubset(sys.argv):
                            print("missing isolated offline build flag", file=sys.stderr)
                            raise SystemExit(91)
                        output = next(
                            value.split("dest=", 1)[1]
                            for value in sys.argv
                            if value.startswith("type=local,dest=")
                        )
                        counter = Path(os.environ["FAKE_PODMAN_COUNTER"])
                        call = int(counter.read_text()) + 1 if counter.exists() else 1
                        counter.write_text(str(call))
                        destination = Path(output)
                        destination.mkdir(parents=True)
                        context = Path(sys.argv[-1])
                        for required_path in (
                            "Containerfile",
                            "SOURCE-MANIFEST.json",
                            "RPM-BUNDLE.sha256",
                            "rpm-bundle/build/build-dependency.rpm",
                            "rpm-bundle/runtime/runtime-dependency.rpm",
                        ):
                            if not (context / required_path).is_file():
                                print("incomplete locked build context", file=sys.stderr)
                                raise SystemExit(92)
                        artifacts = {
                            "lto-ltfs-0.1.0.tar.gz": (
                                context / "lto-ltfs-0.1.0.tar.gz"
                            ).read_bytes(),
                            "lto-ltfs-0.1.0-22.el9.src.rpm": b"source rpm\\n",
                            "lto-ltfs-0.1.0-22.el9.x86_64.rpm": b"binary rpm\\n",
                            "RPM-PAYLOAD-DIGEST": b"sha256:fixture\\n",
                            "SOURCE-MANIFEST.json": (
                                context / "SOURCE-MANIFEST.json"
                            ).read_bytes(),
                        }
                        if os.environ.get("FAKE_PODMAN_MISMATCH") and call == 2:
                            artifacts["lto-ltfs-0.1.0-22.el9.x86_64.rpm"] += b"changed"
                        for name, content in artifacts.items():
                            (destination / name).write_bytes(content)
                        lines = [
                            hashlib.sha256(content).hexdigest() + "  " + name
                            for name, content in artifacts.items()
                        ]
                        (destination / "SHA256SUMS").write_text("\\n".join(lines) + "\\n")
                        """
                    )
                )
                fake_podman.chmod(0o755)
                subprocess.run(["git", "init", "-q"], cwd=checkout, check=True)
                subprocess.run(
                    ["git", "config", "user.email", "test@example.invalid"],
                    cwd=checkout,
                    check=True,
                )
                subprocess.run(
                    ["git", "config", "user.name", "Packaging Test"],
                    cwd=checkout,
                    check=True,
                )
                subprocess.run(["git", "add", "."], cwd=checkout, check=True)
                subprocess.run(
                    ["git", "commit", "-qm", "fixture"], cwd=checkout, check=True
                )
                output = fixture_root / "output"
                environment = os.environ.copy()
                environment.update(
                    {
                        "PATH": str(tools) + os.pathsep + environment["PATH"],
                        "FAKE_PODMAN_COUNTER": str(fixture_root / "counter"),
                    }
                )
                if mismatch:
                    environment["FAKE_PODMAN_MISMATCH"] = "1"
                result = subprocess.run(
                    [
                        str(checkout / BUILD.relative_to(ROOT)),
                        "--rpm-bundle",
                        str(bundle),
                        "--rpm-lock",
                        str(bundle_lock),
                        "--output",
                        str(output),
                    ],
                    cwd=checkout,
                    env=environment,
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual((fixture_root / "counter").read_text(), "2")
                if mismatch:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("not byte reproducible", result.stderr)
                    self.assertFalse(output.exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertTrue((output / "SHA256SUMS").is_file())

    def test_full_build_rejects_missing_or_tampered_rpm_bundle_lock(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        fixture_root = Path(temporary.name)
        checkout = fixture_root / "checkout"
        (checkout / "scripts").mkdir(parents=True)
        shutil.copy2(BUILD, checkout / "scripts/build-rpm.sh")
        shutil.copy2(VERIFY, checkout / "scripts/verify-rpm.py")
        (checkout / "payload.txt").write_text("tracked\n")
        subprocess.run(["git", "init", "-q"], cwd=checkout, check=True)
        subprocess.run(
            ["git", "config", "user.email", "test@example.invalid"],
            cwd=checkout,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Packaging Test"],
            cwd=checkout,
            check=True,
        )
        subprocess.run(["git", "add", "."], cwd=checkout, check=True)
        subprocess.run(["git", "commit", "-qm", "fixture"], cwd=checkout, check=True)

        missing = subprocess.run(
            [
                str(checkout / "scripts/build-rpm.sh"),
                "--output",
                str(fixture_root / "missing"),
            ],
            cwd=checkout,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("--rpm-bundle", missing.stderr)

        bundle = fixture_root / "bundle"
        (bundle / "build").mkdir(parents=True)
        (bundle / "runtime").mkdir()
        (bundle / "build/build.rpm").write_bytes(b"build\n")
        (bundle / "runtime/runtime.rpm").write_bytes(b"runtime\n")
        lock = fixture_root / "RPM-BUNDLE.sha256"
        lock.write_text(
            "0" * 64
            + "  build/build.rpm\n"
            + hashlib.sha256(b"runtime\n").hexdigest()
            + "  runtime/runtime.rpm\n"
        )
        tampered = subprocess.run(
            [
                str(checkout / "scripts/build-rpm.sh"),
                "--rpm-bundle",
                str(bundle),
                "--rpm-lock",
                str(lock),
                "--output",
                str(fixture_root / "tampered"),
            ],
            cwd=checkout,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(tampered.returncode, 0)
        self.assertIn("FAILED", tampered.stdout + tampered.stderr)
        self.assertFalse((fixture_root / "tampered").exists())

    def test_source_archive_is_git_derived_and_reproducible(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        checkout = Path(temporary.name) / "checkout"
        checkout.mkdir()
        (checkout / "scripts").mkdir()
        shutil.copy2(BUILD, checkout / "scripts/build-rpm.sh")
        shutil.copy2(VERIFY, checkout / "scripts/verify-rpm.py")
        (checkout / "payload.txt").write_text("tracked payload\n")
        subprocess.run(["git", "init", "-q"], cwd=checkout, check=True)
        subprocess.run(
            ["git", "config", "user.email", "test@example.invalid"],
            cwd=checkout,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Packaging Test"], cwd=checkout, check=True
        )
        subprocess.run(["git", "add", "."], cwd=checkout, check=True)
        environment = os.environ.copy()
        environment.update(
            {
                "GIT_AUTHOR_DATE": "2026-08-21T00:00:00Z",
                "GIT_COMMITTER_DATE": "2026-08-21T00:00:00Z",
            }
        )
        subprocess.run(
            ["git", "commit", "-qm", "fixture"],
            cwd=checkout,
            env=environment,
            check=True,
        )
        outputs = []
        for name in ("a", "b"):
            output = Path(temporary.name) / name
            subprocess.run(
                [
                    str(checkout / "scripts/build-rpm.sh"),
                    "--source-only",
                    "--output",
                    str(output),
                ],
                cwd=checkout,
                check=True,
                capture_output=True,
                text=True,
            )
            outputs.append(output / "lto-ltfs-0.1.0.tar.gz")
        self.assertEqual(
            hashlib.sha256(outputs[0].read_bytes()).digest(),
            hashlib.sha256(outputs[1].read_bytes()).digest(),
        )
        with tarfile.open(outputs[0], "r:gz") as archive:
            names = archive.getnames()
        self.assertIn("lto-ltfs-0.1.0/payload.txt", names)
        self.assertNotIn("lto-ltfs-0.1.0/untracked.txt", names)
        for output in (path.parent for path in outputs):
            manifest = json.loads((output / "SOURCE-MANIFEST.json").read_text())
            self.assertEqual(manifest["schema"], 1)
            self.assertEqual(
                manifest["archive_sha256"],
                hashlib.sha256(
                    (output / "lto-ltfs-0.1.0.tar.gz").read_bytes()
                ).hexdigest(),
            )


if __name__ == "__main__":
    unittest.main()
