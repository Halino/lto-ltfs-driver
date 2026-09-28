# SPDX-License-Identifier: BSD-3-Clause

import importlib.util
import base64
import hashlib
import inspect
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "bench"


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, BENCH / filename)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class FakeProcess:
    def __init__(self, receipt, readonly, wait_result, operation_id):
        self.receipt, self.readonly, self.wait_result = receipt, readonly, wait_result
        self.operation_id = operation_id
        self.wait_timeouts, self.staged = [], None
        self.completed = False

    def stage_finalization(self, total, files):
        self.staged = (total, files)

    def poll(self):
        return self.wait_result if self.completed else None

    def wait(self, timeout=None):
        self.wait_timeouts.append(timeout)
        was_completed = self.completed
        self.completed = True
        if self.wait_result != 0:
            return self.wait_result
        if self.staged is None:
            if was_completed:
                return 0
            raise AssertionError("wait occurred before unmount finalization")
        total, files = self.staged
        receipt = {"schema": 1, "stage": "terminal",
            "operation_id": self.operation_id,
            "volume_uuid": "22222222-2222-4222-8222-222222222222",
            "prior_generation": 1,
            "new_generation": 1 if self.readonly else 2,
            "bytes_valid": True, "bytes": 0 if self.readonly else total,
            "files_valid": True, "files": 0 if self.readonly else files,
            "phase_duration_ns": [0] * 11, "capture_duration_ns": 0,
            "device_close_duration_ns": 0, "device_close_result_valid": True,
            "device_close_result": 0, "catalog_ack_duration_ns": 0,
            "media_committed": True, "catalog_acknowledged": True,
            "cleanup_failed": False, "result": 0}
        self.receipt.write_text(json.dumps(receipt, separators=(",", ":")) + "\n")
        return 0


class FakeCommands:
    def __init__(self, wait_results=None):
        self.commands, self.image = [], {}
        self.mounted, self.mountpoint = False, None
        self.receipt, self.readonly = None, False
        self.format_config_path, self.format_config_text = None, None
        self.wait_results = list(wait_results or [0, 0])
        self.processes, self.active_process = [], None

    def run(self, argv, **kwargs):
        argv = [str(value) for value in argv]
        self.commands.append(argv)
        if argv[:3] == ["git", "rev-parse", "HEAD"]:
            return subprocess.CompletedProcess(argv, 0, "a" * 40 + "\n", "")
        if argv[:3] == ["git", "status", "--porcelain"]:
            return subprocess.CompletedProcess(argv, 0, "", "")
        if Path(argv[0]).name == "mkltfs":
            self.format_config_path = Path(argv[argv.index("--config") + 1])
            self.format_config_text = self.format_config_path.read_text()
            return subprocess.CompletedProcess(argv, 0, "", "")
        if Path(argv[0]).name == "findmnt":
            return subprocess.CompletedProcess(argv, 0 if self.mounted else 1, "", "")
        if Path(argv[0]).name.startswith("fusermount"):
            total = sum(p.stat().st_size for p in self.mountpoint.iterdir() if p.is_file())
            files = sum(1 for p in self.mountpoint.iterdir() if p.is_file())
            self.image = {p.name: p.read_bytes() for p in self.mountpoint.iterdir() if p.is_file()}
            for path in list(self.mountpoint.iterdir()):
                if path.is_file(): path.unlink()
            self.mounted = False
            self.active_process.stage_finalization(total, files)
            return subprocess.CompletedProcess(argv, 0, "", "")
        raise AssertionError(argv)

    def start(self, argv, **kwargs):
        argv = [str(value) for value in argv]
        self.commands.append(argv)
        self.mountpoint = Path(argv[2])
        options = argv[argv.index("-o") + 1].split(",")
        self.receipt = Path(next(o.split("=", 1)[1] for o in options
                                 if o.startswith("standalone_receipt=")))
        self.readonly = "ro" in options
        for name, payload in self.image.items():
            (self.mountpoint / name).write_bytes(payload)
        self.mounted = True
        index = len(self.processes)
        wait_result = self.wait_results[index] if index < len(self.wait_results) else 0
        process = FakeProcess(
            self.receipt, self.readonly, wait_result, argv[1].split("=", 1)[1]
        )
        self.processes.append(process)
        self.active_process = process
        return process


def make_scheduler_config(root, name="fcfs"):
    plugin = root / f"libiosched-{name}.so"
    plugin.touch()
    return f"plugin iosched {name} {plugin}\ndefault iosched {name}\n"


def validate_config(runner, config, tape_backend, scheduler_name="fcfs"):
    scheduler = Path(config).parent / f"libiosched-{scheduler_name}.so"
    return runner._validate_config_closure(config, tape_backend, scheduler)


def run_benchmark(runner, seed, large_bytes, tools, commands):
    return runner.run(seed, large_bytes, tools, commands,
                      _test_fusermount=tools.fusermount)


class BenchmarkTests(unittest.TestCase):
    def test_cli_rejects_public_fusermount_override(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = root / "missing"
            result = subprocess.run([
                sys.executable, str(BENCH / "run-file-backend.py"),
                "--seed", "1", "--large-bytes", "1024",
                "--output", str(root / "result.json"),
                "--ltfs", str(missing), "--mkltfs", str(missing),
                "--tape-backend", str(missing),
                "--iosched-backend", str(missing),
                "--config", str(missing),
                "--fusermount", str(missing), "--findmnt", str(missing),
            ], text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("unrecognized arguments: --fusermount", result.stderr)

    def test_fusermount_test_seam_is_private_keyword_only(self):
        runner = load("run_file_backend_private_helper", "run-file-backend.py")
        parameter = inspect.signature(runner.run).parameters.get(
            "_test_fusermount"
        )
        self.assertIsNotNone(parameter)
        self.assertEqual(parameter.kind, inspect.Parameter.KEYWORD_ONLY)
        self.assertEqual(runner.PRODUCTION_FUSERMOUNT,
                         Path("/usr/bin/fusermount"))

    def test_production_ignores_toolchain_helper_and_uses_anchored_path(self):
        runner = load("run_file_backend_production_helper",
                      "run-file-backend.py")
        commands = FakeCommands()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = self.make_tools(root, runner)
            attacker = tools.fusermount
            production = root / "fusermount-production"
            production.touch(mode=0o700)
            runner.PRODUCTION_FUSERMOUNT = production
            result = runner.run(19, 1024 * 1024, tools, commands)
        self.assertTrue(result["validation"]["all_hashes_match"])
        helpers = [Path(command[0]) for command in commands.commands
                   if Path(command[0]).name.startswith("fusermount")]
        self.assertTrue(helpers)
        self.assertTrue(all(helper == production for helper in helpers))
        self.assertNotIn(attacker, helpers)

    def test_production_rejects_symlinked_fusermount_helper(self):
        runner = load("run_file_backend_production_helper_symlink",
                      "run-file-backend.py")
        commands = FakeCommands()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = self.make_tools(root, runner)
            target = root / "fusermount-target"
            target.touch(mode=0o700)
            production = root / "fusermount-production"
            production.symlink_to(target.name)
            runner.PRODUCTION_FUSERMOUNT = production
            with self.assertRaisesRegex(RuntimeError,
                                        "production fusermount is invalid"):
                runner.run(20, 1024 * 1024, tools, commands)
        self.assertFalse(any(Path(command[0]).name == "mkltfs"
                             for command in commands.commands))

    def make_attestation_repo(self, root, mutate_bytecode_guard=False):
        bench = root / "bench"
        bench.mkdir()
        runner_source = (BENCH / "run-file-backend.py").read_text()
        if mutate_bytecode_guard:
            runner_source = runner_source.replace("sys.dont_write_bytecode = True\n", "")
        (bench / "run-file-backend.py").write_text(runner_source)
        shutil.copy2(BENCH / "workloads.py", bench / "workloads.py")
        tool_names = ("ltfs", "mkltfs", "libtape-file.so",
                      "libiosched-fcfs.so", "ltfs.conf", "fusermount",
                      "findmnt")
        for name in tool_names:
            (root / name).write_text(name + "\n")
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.name", "LTFS test"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.email", "ltfs@example.invalid"],
                       cwd=root, check=True)
        subprocess.run(["git", "add", "."], cwd=root, check=True)
        subprocess.run(["git", "commit", "-qm", "fixture"], cwd=root, check=True)

    def run_attestation_probe(self, root):
        probe = """
from pathlib import Path
root = Path.cwd()
path = root / 'bench/run-file-backend.py'
namespace = {'__file__': str(path), '__name__': 'attestation_probe'}
exec(compile(path.read_bytes(), str(path), 'exec'), namespace)
tools = namespace['Toolchain'](*(root / name for name in (
    'ltfs', 'mkltfs', 'libtape-file.so', 'libiosched-fcfs.so', 'ltfs.conf',
    'fusermount', 'findmnt')))
namespace['_build'](root, tools, namespace['Commands']())
"""
        environment = os.environ.copy()
        environment.pop("PYTHONDONTWRITEBYTECODE", None)
        return subprocess.run([sys.executable, "-c", probe], cwd=root,
                              text=True, capture_output=True, env=environment)

    def test_import_does_not_dirty_real_git_attestation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_attestation_repo(root)
            result = self.run_attestation_probe(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            status = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                                    text=True, capture_output=True, check=True)
            self.assertEqual(status.stdout, "")

    def test_bytecode_guard_mutation_and_preexisting_dirt_fail(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_attestation_repo(root, mutate_bytecode_guard=True)
            result = self.run_attestation_probe(root)
            self.assertNotEqual(result.returncode, 0)
            status = subprocess.run(["git", "status", "--porcelain"], cwd=root,
                                    text=True, capture_output=True, check=True)
            self.assertIn("bench/__pycache__/", status.stdout)
        for dirty_kind in ("tracked", "untracked"):
            with self.subTest(dirty_kind=dirty_kind), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self.make_attestation_repo(root)
                if dirty_kind == "tracked":
                    (root / "ltfs").write_text("modified\n")
                else:
                    (root / "preexisting-untracked").write_text("dirty\n")
                result = self.run_attestation_probe(root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("benchmark requires an exact clean commit", result.stderr)

    def test_build_attestation_scheduler_digest_is_byte_sensitive(self):
        runner = load("run_file_backend_iosched_digest", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = self.make_tools(root, runner)
            tools.iosched_backend.write_bytes(b"scheduler build one")
            first = runner._build(ROOT, tools, FakeCommands())
            tools.iosched_backend.write_bytes(b"scheduler build two")
            second = runner._build(ROOT, tools, FakeCommands())
        first_digest = first["artifacts"].get("iosched_backend")
        second_digest = second["artifacts"].get("iosched_backend")
        self.assertRegex(first_digest or "", r"^[0-9a-f]{64}$")
        self.assertRegex(second_digest or "", r"^[0-9a-f]{64}$")
        self.assertNotEqual(first_digest, second_digest)

    def test_workloads_are_seeded_bounded_and_repeatable(self):
        workloads = load("lto_workloads", "workloads.py")
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            left = workloads.generate(Path(first), 20260821, large_bytes=1024 * 1024)
            right = workloads.generate(Path(second), 20260821, large_bytes=1024 * 1024)
            self.assertEqual(left, right)
            self.assertEqual([item["size_bytes"] for item in left], [
                1024, 16 * 1024, 256 * 1024, 4 * 1024 * 1024, 1024 * 1024,
            ])

    def make_tools(self, root, runner):
        tools = runner.Toolchain(*[root / name for name in (
            "ltfs", "mkltfs", "libtape-file.so", "libiosched-fcfs.so",
            "ltfs.conf", "fusermount", "findmnt"
        )])
        for path in tools: path.touch(mode=0o700)
        local_config = root / "ltfs.conf.local"
        local_config.write_text("# benchmark-local-settings\n")
        tools.config.write_text(
            f"plugin tape file {tools.tape_backend}\n"
            "default tape file\n"
            f"plugin iosched fcfs {tools.iosched_backend}\n"
            "default iosched fcfs\n"
            "default kmi none\n"
            f"include {local_config}\n"
        )
        return tools

    def test_real_mount_unmount_remount_boundary_is_required(self):
        runner = load("run_file_backend", "run-file-backend.py")
        commands = FakeCommands()
        with tempfile.TemporaryDirectory() as temporary:
            tools = self.make_tools(Path(temporary), runner)
            result = run_benchmark(runner, 20260821, 1024 * 1024,
                                   tools, commands)
        format_commands = [command for command in commands.commands
                           if Path(command[0]).name == "mkltfs"]
        self.assertEqual(len(format_commands), 1)
        self.assertEqual(format_commands[0][1], "--config")
        self.assertEqual(format_commands[0][3:5], ["--backend", "file"])
        self.assertEqual(commands.format_config_path, tools.config)
        self.assertIn(f"plugin tape file {tools.tape_backend}\n",
                      commands.format_config_text)
        self.assertIn(
            f"plugin iosched fcfs {tools.config.with_name('libiosched-fcfs.so')}\n",
            commands.format_config_text,
        )
        self.assertIn("default iosched fcfs\n", commands.format_config_text)
        self.assertIn("default kmi none\n", commands.format_config_text)
        self.assertIn(f"include {tools.config.with_name('ltfs.conf.local')}\n",
                      commands.format_config_text)
        mount_commands = [command for command in commands.commands
                          if Path(command[0]).name == "ltfs"]
        self.assertEqual(len(mount_commands), 2)
        for mount_command in mount_commands:
            self.assertRegex(
                mount_command[1], r"^--operation-id=[0-9a-f-]{36}$"
            )
            options = mount_command[mount_command.index("-o") + 1].split(",")
            self.assertIn("tape_backend=file", options)
            self.assertIn(f"config_file={commands.format_config_path}", options)
        self.assertEqual(result["validation"]["method"], "ltfs-unmount-remount-index-hash")
        self.assertTrue(result["validation"]["all_hashes_match"])
        self.assertEqual(sum(Path(cmd[0]).name == "ltfs" for cmd in commands.commands), 2)
        self.assertEqual(sum(Path(cmd[0]).name.startswith("fusermount") for cmd in commands.commands), 2)
        self.assertEqual(len(commands.processes), 2)
        self.assertEqual([process.wait_timeouts for process in commands.processes],
                         [[30], [30]])
        self.assertFalse(result["backend"]["physical_tape"])
        self.assertFalse(result["build"]["dirty"])
        self.assertEqual([item["purpose"] for item in result["validation"]["receipts"]],
                         ["write", "readback"])
        self.assertTrue(all("content_base64" in item
                            for item in result["validation"]["receipts"]))

    def test_configuration_closure_is_validated_before_format(self):
        runner = load("run_file_backend_config_closure", "run-file-backend.py")
        for mutation in ("missing_include", "wrong_tape_plugin"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as temporary:
                commands = FakeCommands()
                tools = self.make_tools(Path(temporary), runner)
                if mutation == "missing_include":
                    tools.config.with_name("ltfs.conf.local").unlink()
                else:
                    tools.config.write_text(
                        f"plugin tape file {tools.ltfs}\n"
                        "default tape file\n"
                        f"{make_scheduler_config(Path(temporary))}"
                        "default kmi none\n"
                    )
                with self.assertRaisesRegex(RuntimeError,
                                            "LTFS configuration closure is invalid"):
                    run_benchmark(runner, 20260821, 1024 * 1024,
                                  tools, commands)
                self.assertFalse(any(Path(command[0]).name == "mkltfs"
                                     for command in commands.commands))

    def test_scheduler_plugin_must_match_independent_exact_regular_path(self):
        runner = load("run_file_backend_scheduler_authority",
                      "run-file-backend.py")
        for mutation in ("wrong", "hardlink_alias", "dotdot_alias", "symlink"):
            with self.subTest(mutation=mutation), \
                    tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                commands = FakeCommands()
                tools = self.make_tools(root, runner)
                if mutation == "wrong":
                    configured = root / "wrong-iosched.so"
                    configured.write_bytes(b"wrong scheduler")
                elif mutation == "hardlink_alias":
                    configured = root / "hardlink-iosched.so"
                    os.link(tools.iosched_backend, configured)
                elif mutation == "dotdot_alias":
                    (root / "alias-dir").mkdir()
                    configured = root / "alias-dir" / ".." / tools.iosched_backend.name
                else:
                    configured = root / "symlink-iosched.so"
                    configured.symlink_to(tools.iosched_backend.name)
                    tools = tools._replace(iosched_backend=configured)
                tools.config.write_text(
                    f"plugin tape file {tools.tape_backend}\n"
                    f"plugin iosched fcfs {configured}\n"
                    "default tape file\n"
                    "default iosched fcfs\n"
                    "default kmi none\n"
                )
                with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                    run_benchmark(runner, 20260821, 1024 * 1024,
                                  tools, commands)
                self.assertFalse(any(Path(command[0]).name == "mkltfs"
                                     for command in commands.commands))

    def test_configuration_closure_matches_inline_include_order_and_removals(self):
        runner = load("run_file_backend_config_semantics", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backend = root / "libtape-file.so"
            wrong = root / "wrong.so"
            backend.touch()
            wrong.touch()
            scheduler = make_scheduler_config(root)

            first = root / "first.conf"
            second = root / "second.conf"
            first.write_text(f"plugin tape file {backend}\n")
            second.write_text(f"plugin tape file {wrong}\n")
            config = root / "order.conf"
            config.write_text(
                f"include {first}\ninclude {second}\ndefault tape file\n"
                f"{scheduler}"
            )
            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, backend)

            local = root / "local.conf"
            local.write_text("-plugin tape file\n")
            config.write_text(
                f"plugin tape file {backend}\ndefault tape file\n"
                f"{scheduler}include {local}\n"
            )
            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, backend)

            local.write_text("-default tape\n")
            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, backend)

    def test_configuration_closure_comments_repeated_includes_and_cycles(self):
        runner = load("run_file_backend_config_edges", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backend = root / "libtape-file.so"
            backend.touch()
            scheduler = make_scheduler_config(root)
            local = root / "local.conf"
            local.write_text("-default tape\ndefault tape file # restored\n")
            config = root / "ltfs.conf"
            config.write_text(
                f"plugin tape file {backend} # exact implementation\n"
                "default tape file\n"
                f"{scheduler}"
                f"include {local}\ninclude {local}\n"
                f"include_noerror {root / 'absent.conf'}\n"
            )
            validate_config(runner, config, backend)

            optional = root / "optional.conf"
            optional.write_text("-default missing\n")
            config.write_text(
                f"plugin tape file {backend}\ndefault tape file\n"
                f"{scheduler}"
                f"include_noerror {optional}\n"
            )
            validate_config(runner, config, backend)

            config.write_text(
                f"plugin tape file {backend}\ndefault tape file\n"
                f"{scheduler}include {local}\n"
            )
            local.write_text(f"include {config}\n")
            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, backend)

    def test_optional_include_stops_at_the_same_parser_errors_as_ltfs(self):
        runner = load("run_file_backend_config_optional", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = root / "libtape-file.so"
            wrong = root / "wrong.so"
            expected.touch()
            wrong.touch()
            scheduler = make_scheduler_config(root)
            optional = root / "optional.conf"
            config = root / "ltfs.conf"
            config.write_text(
                f"plugin tape file {wrong}\ninclude_noerror {optional}\n"
                "default tape file\n"
                f"{scheduler}"
            )
            for malformed in ("option\n", "x" * 65534 + "\n"):
                with self.subTest(malformed=malformed[:16]):
                    optional.write_text(
                        malformed + f"plugin tape file {expected}\n"
                    )
                    with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                        validate_config(runner, config, expected)

    def test_include_requires_exactly_one_c_parser_token(self):
        runner = load("run_file_backend_config_include_arity",
                      "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = root / "libtape-file.so"
            wrong = root / "wrong.so"
            expected.touch()
            wrong.touch()
            scheduler = make_scheduler_config(root)
            remap = root / "remap with spaces.conf"
            remap.write_text(f"plugin tape file {expected}\n")
            optional = root / "optional.conf"
            optional.write_text(f"include {remap}\n")
            config = root / "ltfs.conf"
            config.write_text(
                f"plugin tape file {wrong}\ninclude_noerror {optional}\n"
                "default tape file\n"
                f"{scheduler}"
            )

            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, expected)

            remap.write_text(f"plugin tape file {wrong}\n")
            config.write_text(
                f"plugin tape file {expected}\ninclude_noerror {optional}\n"
                "default tape file\n"
                f"{scheduler}"
            )
            validate_config(runner, config, expected)

    def test_configuration_requires_a_real_supported_scheduler_default(self):
        runner = load("run_file_backend_config_scheduler", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backend = root / "libtape-file.so"
            backend.touch()
            config = root / "ltfs.conf"
            base = (
                f"plugin tape file {backend}\n"
                "default tape file\n"
                "default kmi none\n"
            )
            custom = root / "libiosched-custom.so"
            custom.touch()
            for scheduler in (
                "default iosched none\n",
                "",
                "default iosched fcfs\n",
                f"plugin iosched custom {custom}\ndefault iosched custom\n",
            ):
                with self.subTest(scheduler=scheduler):
                    config.write_text(base + scheduler)
                    with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                        validate_config(runner, config, backend)

            for name in ("fcfs", "unified"):
                with self.subTest(name=name):
                    config.write_text(base + make_scheduler_config(root, name))
                    validate_config(runner, config, backend, name)

    def test_plugin_path_preserves_c_delimiter_switch(self):
        runner = load("run_file_backend_config_plugin_delimiter",
                      "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = root / "expected.so"
            expected.write_text("expected authority\n")
            c_relative = Path(f"  {expected}")
            c_authority = root / c_relative
            c_authority.parent.mkdir(parents=True)
            c_authority.write_text("C parser authority\n")
            config = root / "ltfs.conf"
            config.write_text(
                f"plugin tape file   {expected}\ndefault tape file\n"
            )
            self.assertNotEqual(expected.resolve(), c_authority.resolve())
            previous = Path.cwd()
            try:
                os.chdir(root)
                with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                    validate_config(runner, config, expected)
            finally:
                os.chdir(previous)

    def test_config_authority_matches_c_tokenizer_matrix(self):
        runner = load("run_file_backend_config_c_oracle",
                      "run-file-backend.py")
        separators = (b" ", b"\t", b"  ", b"\t ", b" \t")
        prefixes = (b"", b" ", b"\t", b" \t")
        trailers = (b"", b" ", b"\t", b" \t", b"\r\n")
        lines = []
        for prefix in prefixes:
            for first in separators:
                for second in separators:
                    for third in separators:
                        lines.append(prefix + b"plugin" + first + b"tape" +
                                     second + b"file" + third +
                                     b"/authority.so")
                    lines.append(prefix + b"default" + first + b"tape" +
                                 second + b"file")
                for directive in (b"include", b"include_noerror"):
                    lines.append(prefix + directive + first + b"/authority.conf")
        for trailer in trailers:
            lines.extend((
                b"plugin tape file /authority.so" + trailer,
                b"default tape file" + trailer,
                b"include /authority.conf" + trailer,
                b"include_noerror /authority.conf" + trailer,
            ))
        lines.extend((
            b"plugin", b"plugin tape", b"plugin tape file",
            b"default", b"default tape", b"default tape file extra",
            b"include", b"include /one /two",
            b"include_noerror", b"include_noerror /one /two",
            b"plugin tape file /authority.so \t # comment",
            b"plugin tape file   /authority.so#comment",
            b"default tape file \t#comment",
            b"include /authority.conf#comment",
            b"include_noerror /authority.conf \t#comment",
        ))

        with tempfile.TemporaryDirectory() as temporary:
            oracle = Path(temporary) / "config-tokenizer-oracle"
            compiler = shlex.split(os.environ.get("CC", "cc"))
            build = subprocess.run([
                *compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                str(ROOT / "tests/c/config_tokenizer_oracle.c"),
                "-o", str(oracle),
            ], text=True, capture_output=True, check=False)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run(
                [str(oracle), *(line.hex() for line in lines)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
        c_results = result.stdout.splitlines()
        self.assertEqual(len(c_results), len(lines))

        def python_result(line):
            try:
                authority = runner._config_parse_authority(line)
            except RuntimeError:
                return "ERR"
            if authority is None:
                return "IGNORE"
            return "OK" + "".join(
                ":" + field.encode().hex() for field in authority
            )

        for line, c_result in zip(lines, c_results):
            with self.subTest(line=line):
                self.assertEqual(python_result(line), c_result)

    def test_validator_only_policy_errors_are_never_suppressed(self):
        runner = load("run_file_backend_config_policy", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = root / "libtape-file.so"
            wrong = root / "wrong.so"
            expected.touch()
            wrong.touch()
            scheduler = make_scheduler_config(root)
            optional = root / "optional.conf"
            config = root / "ltfs.conf"
            config.write_text(
                f"plugin tape file {expected}\ninclude_noerror {optional}\n"
                "default tape file\n"
                f"{scheduler}"
            )
            for prefix in (b"\xff\n", b"\0\n"):
                with self.subTest(prefix=prefix):
                    optional.write_bytes(
                        prefix + f"plugin tape file {wrong}\n".encode()
                    )
                    with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                        validate_config(runner, config, expected)

            relative = root / "relative.conf"
            relative.write_text(f"plugin tape file {wrong}\n")
            optional.write_text("include relative.conf\n")
            previous = Path.cwd()
            try:
                os.chdir(root)
                with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                    validate_config(runner, config, expected)
            finally:
                os.chdir(previous)

    def test_config_tokenizer_uses_only_c_ascii_delimiters(self):
        runner = load("run_file_backend_config_ascii_tokens", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            expected = root / "libtape-file.so"
            wrong = root / "wrong.so"
            expected.touch()
            wrong.touch()
            scheduler = make_scheduler_config(root)
            optional = root / "optional.conf"
            config = root / "ltfs.conf"
            config.write_text(
                f"plugin tape file {wrong}\ninclude_noerror {optional}\n"
                "default tape file\n"
                f"{scheduler}"
            )
            remap = f"plugin tape file {expected}\n".encode()
            for prefix in (b"\v", b"\f", "\N{NO-BREAK SPACE}".encode("utf-8")):
                with self.subTest(prefix=prefix):
                    optional.write_bytes(prefix + remap)
                    with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                        validate_config(runner, config, expected)
            optional.write_bytes(
                b"\tplugin\ttape\tfile\t" + os.fsencode(expected) + b" \r\n"
            )
            validate_config(runner, config, expected)

    def test_configuration_closure_has_explicit_file_and_byte_bounds(self):
        runner = load("run_file_backend_config_bounds", "run-file-backend.py")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            backend = root / "libtape-file.so"
            backend.touch()
            config = root / "ltfs.conf"

            includes = []
            for index in range(runner.MAX_CONFIG_FILES):
                include = root / f"file-{index}.conf"
                include.write_text("")
                includes.append(f"include {include}\n")
            config.write_text(
                f"plugin tape file {backend}\ndefault tape file\n"
                + "".join(includes)
            )
            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, backend)

            includes = []
            for index in range(runner.MAX_CONFIG_FILES - 1):
                include = root / f"large-{index}.conf"
                include.write_bytes(b"# bounded padding\n" * 4200)
                includes.append(f"include {include}\n")
            config.write_text(
                f"plugin tape file {backend}\ndefault tape file\n"
                + "".join(includes)
            )
            with self.assertRaisesRegex(RuntimeError, "closure is invalid"):
                validate_config(runner, config, backend)

    def test_main_keeps_the_file_exception_before_the_physical_guard(self):
        source = (ROOT / "src/main.c").read_text()
        requirement = source.index("ltfs_device_guard_requirement(")
        physical = source.index("ltfs_device_config_load(", requirement)
        plugin = source.index("plugin_load(&priv->tape_plugin", physical)
        cleanup_label = source.index("cleanup_after_lock:", plugin)
        release = source.index("ltfs_device_guard_release_preserving_result(",
                               cleanup_label)
        self.assertLess(requirement, physical)
        self.assertLess(physical, plugin)
        self.assertLess(plugin, cleanup_label)
        self.assertLess(cleanup_label, release)
        self.assertIn("if (ret > 0)", source[requirement:physical])

    def test_nonzero_finalization_wait_fails_without_result(self):
        runner = load("run_file_backend_wait_failure", "run-file-backend.py")
        for wait_results, expected_processes in (([1], 1), ([0, 1], 2)):
            with self.subTest(wait_results=wait_results):
                commands = FakeCommands(wait_results=wait_results)
                result = None
                with tempfile.TemporaryDirectory() as temporary:
                    with self.assertRaises(RuntimeError):
                        tools = self.make_tools(Path(temporary), runner)
                        result = run_benchmark(
                            runner, 9, 1024 * 1024, tools, commands
                        )
                self.assertIsNone(result)
                self.assertEqual(len(commands.processes), expected_processes)
                self.assertEqual(commands.processes[-1].wait_timeouts, [30, 30])
                self.assertTrue(all(process.wait_timeouts == [30]
                                    for process in commands.processes[:-1]))

    def test_write_and_read_mount_readiness_failures_cleanup_late_mount(self):
        runner = load("run_file_backend_mount_readiness_cleanup",
                      "run-file-backend.py")

        class ReadinessFailureCommands(FakeCommands):
            def __init__(self, fail_index):
                super().__init__()
                self.fail_index = fail_index
                self.failed_process = None
                self.readiness_probes = 0
                self.cleanup_started = False

            def start(self, argv, **kwargs):
                process = super().start(argv, **kwargs)
                if len(self.processes) - 1 == self.fail_index:
                    self.failed_process = process
                    self.mounted = False
                return process

            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                failing = (self.failed_process is not None and
                           self.active_process is self.failed_process)
                if (failing and Path(normalized[0]).name == "findmnt" and
                        not self.cleanup_started):
                    self.commands.append(normalized)
                    self.readiness_probes += 1
                    return subprocess.CompletedProcess(normalized, 1, "", "")
                if failing and Path(normalized[0]).name.startswith("fusermount"):
                    self.commands.append(normalized)
                    self.cleanup_started = True
                    if "-z" in normalized:
                        self.mounted = False
                        self.failed_process.stage_finalization(0, 0)
                        return subprocess.CompletedProcess(normalized, 0, "", "")
                    self.mounted = True
                    return subprocess.CompletedProcess(
                        normalized, 1, "", "late mount"
                    )
                return super().run(argv, **kwargs)

        original_sleep = runner.time.sleep
        runner.time.sleep = lambda _seconds: None
        try:
            for fail_index in (0, 1):
                with self.subTest(fail_index=fail_index), \
                        tempfile.TemporaryDirectory() as temporary:
                    commands = ReadinessFailureCommands(fail_index)
                    with self.assertRaisesRegex(
                            RuntimeError,
                            "LTFS mount state did not reach the required boundary"):
                        tools = self.make_tools(Path(temporary), runner)
                        run_benchmark(runner, 15 + fail_index, 1024 * 1024,
                                      tools, commands)
                    self.assertEqual(commands.readiness_probes, 100)
                    self.assertFalse(commands.mounted)
                    self.assertEqual(commands.failed_process.wait_timeouts, [30])
                    unmounts = [
                        command for command in commands.commands
                        if Path(command[0]).name.startswith("fusermount")
                    ][fail_index:]
                    self.assertEqual([command[1:-1] for command in unmounts],
                                     [["-u"], ["-u", "-z"]])
        finally:
            runner.time.sleep = original_sleep

    def test_deleting_process_wait_is_detected_by_missing_receipt(self):
        runner = load("run_file_backend_wait_mutant", "run-file-backend.py")
        commands = FakeCommands()
        def unmount_without_wait(mountpoint, process, tools, command_runner):
            runner._checked(command_runner.run(
                [str(tools.fusermount), "-u", str(mountpoint)]), "LTFS unmount")
            runner._wait_mount(mountpoint, tools, command_runner, False)
        runner._unmount = unmount_without_wait
        result = None
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises((OSError, RuntimeError)):
                tools = self.make_tools(Path(temporary), runner)
                result = run_benchmark(runner, 10, 1024 * 1024,
                                       tools, commands)
        self.assertIsNone(result)
        self.assertEqual(commands.processes[0].wait_timeouts, [])

    def test_readback_unmount_failure_enters_cleanup_and_reaps_child(self):
        runner = load("run_file_backend_readback_unmount_cleanup",
                      "run-file-backend.py")

        class ReadbackUnmountFailureCommands(FakeCommands):
            def __init__(self):
                super().__init__()
                self.readback_normal_attempts = 0

            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if (Path(normalized[0]).name.startswith("fusermount") and
                        self.active_process is not None and
                        self.active_process.readonly and "-z" not in normalized):
                    self.readback_normal_attempts += 1
                    if self.readback_normal_attempts == 1:
                        self.commands.append(normalized)
                        return subprocess.CompletedProcess(
                            normalized, 1, "", "normal unmount failed"
                        )
                return super().run(argv, **kwargs)

        commands = ReadbackUnmountFailureCommands()
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "LTFS unmount failed"):
                tools = self.make_tools(Path(temporary), runner)
                run_benchmark(runner, 13, 1024 * 1024, tools, commands)
        self.assertEqual(commands.readback_normal_attempts, 2)
        self.assertFalse(commands.mounted)
        self.assertEqual(commands.processes[1].wait_timeouts, [30])

    def test_hash_mismatch_cleanup_waits_for_empty_mount_and_reaps_live_child(self):
        runner = load("run_file_backend_hash_cleanup", "run-file-backend.py")

        class MountedAfterNormalCleanupCommands(FakeCommands):
            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if (Path(normalized[0]).name.startswith("fusermount") and
                        self.active_process is not None and
                        self.active_process.readonly and "-z" not in normalized):
                    self.commands.append(normalized)
                    return subprocess.CompletedProcess(normalized, 0, "", "")
                return super().run(argv, **kwargs)

            def start(self, argv, **kwargs):
                process = super().start(argv, **kwargs)
                if process.readonly:
                    first = next(path for path in self.mountpoint.iterdir()
                                 if path.is_file())
                    first.write_bytes(first.read_bytes() + b"corrupt")
                return process

        commands = MountedAfterNormalCleanupCommands()
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(
                    RuntimeError, "LTFS remount hash validation failed"):
                tools = self.make_tools(Path(temporary), runner)
                run_benchmark(runner, 14, 1024 * 1024, tools, commands)
        readback_unmounts = [
            command for command in commands.commands
            if (Path(command[0]).name.startswith("fusermount") and
                commands.processes[1].readonly)
        ][1:]
        self.assertEqual([command[1:-1] for command in readback_unmounts],
                         [["-u"], ["-u", "-z"]])
        self.assertFalse(commands.mounted)
        self.assertEqual(commands.processes[1].wait_timeouts, [30])

    def test_cleanup_confirms_empty_mount_after_lazy_and_reaps_dead_child(self):
        runner = load("run_file_backend_cleanup_completion",
                      "run-file-backend.py")

        class CleanupCommands:
            def __init__(self):
                self.calls = []
                self.mounted = True

            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if Path(normalized[0]).name == "findmnt":
                    self.calls.append("mounted" if self.mounted else "empty")
                    return subprocess.CompletedProcess(
                        normalized, 0 if self.mounted else 1, "", ""
                    )
                if "-z" in normalized:
                    self.calls.append("lazy")
                    self.mounted = False
                else:
                    self.calls.append("normal")
                return subprocess.CompletedProcess(normalized, 0, "", "")

        class DeadProcess:
            def __init__(self):
                self.wait_timeouts = []

            def wait(self, timeout=None):
                self.wait_timeouts.append(timeout)
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            commands = CleanupCommands()
            process = DeadProcess()
            tools = self.make_tools(root, runner)
            runner._cleanup_mount(root / "mount", process, tools, commands)
        self.assertEqual(commands.calls,
                         ["normal", "mounted", "lazy", "empty", "empty"])
        self.assertEqual(process.wait_timeouts, [30])

    def test_write_and_readback_cleanup_detach_remount_during_process_wait(self):
        runner = load("run_file_backend_cleanup_wait_remount",
                      "run-file-backend.py")

        class RemountDuringWaitCommands(FakeCommands):
            def __init__(self, target_index):
                super().__init__()
                self.target_index = target_index
                self.target_process = None
                self.target_unmounts = []
                self.remounts_during_wait = 0

            def start(self, argv, **kwargs):
                process = super().start(argv, **kwargs)
                if len(self.processes) - 1 == self.target_index:
                    self.target_process = process
                    original_wait = process.wait

                    def wait(timeout=None):
                        result = original_wait(timeout=timeout)
                        if self.remounts_during_wait == 0:
                            self.mounted = True
                            self.remounts_during_wait += 1
                        return result

                    process.wait = wait
                    if process.readonly:
                        first = next(path for path in self.mountpoint.iterdir()
                                     if path.is_file())
                        first.write_bytes(first.read_bytes() + b"corrupt")
                return process

            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if (Path(normalized[0]).name.startswith("fusermount") and
                        self.active_process is self.target_process):
                    self.target_unmounts.append(normalized)
                    if "-z" not in normalized:
                        self.commands.append(normalized)
                        return subprocess.CompletedProcess(
                            normalized, 1, "", "normal unmount failed"
                        )
                return super().run(argv, **kwargs)

        original_copy = runner.shutil.copyfileobj

        def fail_write(*_args, **_kwargs):
            raise RuntimeError("primary write failure")

        try:
            for target_index, primary_message in (
                    (0, "primary write failure"),
                    (1, "LTFS remount hash validation failed")):
                with self.subTest(target_index=target_index), \
                        tempfile.TemporaryDirectory() as temporary:
                    commands = RemountDuringWaitCommands(target_index)
                    if target_index == 0:
                        runner.shutil.copyfileobj = fail_write
                    else:
                        runner.shutil.copyfileobj = original_copy
                    with self.assertRaisesRegex(RuntimeError, primary_message):
                        tools = self.make_tools(Path(temporary), runner)
                        run_benchmark(runner, 21 + target_index,
                                      1024 * 1024, tools, commands)
                    self.assertFalse(commands.mounted)
                    self.assertEqual(commands.remounts_during_wait, 1)
                    self.assertEqual(commands.target_process.wait_timeouts,
                                     [30])
                    self.assertEqual(
                        [command[1:-1] for command in commands.target_unmounts],
                        [["-u"], ["-u", "-z"], ["-u", "-z"]],
                    )
                    self.assertEqual(Path(commands.commands[-1][0]).name,
                                     "findmnt")
        finally:
            runner.shutil.copyfileobj = original_copy

    def test_findmnt_operational_error_is_never_unmounted_proof(self):
        runner = load("run_file_backend_findmnt_rc2", "run-file-backend.py")

        class FindmntFailureCommands:
            def run(self, argv, **kwargs):
                return subprocess.CompletedProcess(argv, 2, "", "findmnt failed")

        original_sleep = runner.time.sleep
        runner.time.sleep = lambda _seconds: None
        try:
            for expected in (True, False):
                with self.subTest(expected=expected), self.assertRaisesRegex(
                        RuntimeError, "findmnt failed"):
                    runner._wait_mount(Path("/mount"),
                                       type("Tools", (), {"findmnt": "/findmnt"})(),
                                       FindmntFailureCommands(), expected)
        finally:
            runner.time.sleep = original_sleep

    def test_cleanup_escalates_stubborn_child_to_kill_and_final_wait(self):
        runner = load("run_file_backend_stubborn_child", "run-file-backend.py")

        class UnmountedCommands:
            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if Path(normalized[0]).name == "findmnt":
                    return subprocess.CompletedProcess(normalized, 1, "", "")
                return subprocess.CompletedProcess(normalized, 0, "", "")

        class StubbornProcess:
            def __init__(self):
                self.wait_timeouts = []
                self.terminate_calls = 0
                self.kill_calls = 0

            def wait(self, timeout=None):
                self.wait_timeouts.append(timeout)
                if self.kill_calls:
                    return -9
                raise subprocess.TimeoutExpired("ltfs", timeout)

            def terminate(self):
                self.terminate_calls += 1

            def kill(self):
                self.kill_calls += 1

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = self.make_tools(root, runner)
            process = StubbornProcess()
            runner._cleanup_mount(root / "mount", process, tools,
                                  UnmountedCommands())
        self.assertEqual(process.wait_timeouts, [30, 5, 5])
        self.assertEqual(process.terminate_calls, 1)
        self.assertEqual(process.kill_calls, 1)

    def test_process_wait_error_still_escalates_through_kill_and_reap(self):
        runner = load("run_file_backend_wait_error_containment",
                      "run-file-backend.py")

        class UnmountedCommands:
            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                return subprocess.CompletedProcess(
                    normalized,
                    1 if Path(normalized[0]).name == "findmnt" else 0,
                    "", "",
                )

        class WaitErrorProcess:
            def __init__(self, error_index):
                self.error_index = error_index
                self.wait_timeouts = []
                self.terminate_calls = 0
                self.kill_calls = 0

            def wait(self, timeout=None):
                index = len(self.wait_timeouts)
                self.wait_timeouts.append(timeout)
                if index == self.error_index:
                    raise OSError("wait failed")
                if index < 2:
                    raise subprocess.TimeoutExpired("ltfs", timeout)
                return -9

            def terminate(self):
                self.terminate_calls += 1

            def kill(self):
                self.kill_calls += 1

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = self.make_tools(root, runner)
            for error_index in (0, 1):
                with self.subTest(error_index=error_index):
                    process = WaitErrorProcess(error_index)
                    observed_error = None
                    try:
                        runner._cleanup_mount(root / "mount", process, tools,
                                              UnmountedCommands())
                    except RuntimeError as error:
                        observed_error = error
                    self.assertIsNone(observed_error)
                    self.assertEqual(process.wait_timeouts, [30, 5, 5])
                    self.assertEqual(process.terminate_calls, 1)
                    self.assertEqual(process.kill_calls, 1)

    def test_cleanup_rejects_rc2_and_mount_remaining_after_lazy(self):
        runner = load("run_file_backend_cleanup_proof", "run-file-backend.py")

        class CleanupProofCommands:
            def __init__(self, findmnt_returncode):
                self.findmnt_returncode = findmnt_returncode

            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                returncode = (self.findmnt_returncode
                              if Path(normalized[0]).name == "findmnt" else 0)
                return subprocess.CompletedProcess(normalized, returncode, "", "")

        class ReapedProcess:
            def wait(self, timeout=None):
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tools = self.make_tools(root, runner)
            for returncode in (0, 2):
                with self.subTest(returncode=returncode), self.assertRaisesRegex(
                        RuntimeError, "cleanup failed"):
                    runner._cleanup_mount(
                        root / "mount", ReapedProcess(), tools,
                        CleanupProofCommands(returncode), force_lazy=True
                    )

    def test_cleanup_failure_is_chained_without_masking_primary_error(self):
        runner = load("run_file_backend_cleanup_chain", "run-file-backend.py")

        class CorruptReadbackCommands(FakeCommands):
            def start(self, argv, **kwargs):
                process = super().start(argv, **kwargs)
                if process.readonly:
                    first = next(path for path in self.mountpoint.iterdir()
                                 if path.is_file())
                    first.write_bytes(first.read_bytes() + b"corrupt")
                return process

        original_cleanup = runner._cleanup_mount

        def fail_cleanup(*args, **kwargs):
            raise RuntimeError("cleanup failed: mount remains active")

        runner._cleanup_mount = fail_cleanup
        try:
            with tempfile.TemporaryDirectory() as temporary:
                with self.assertRaises(RuntimeError) as raised:
                    tools = self.make_tools(Path(temporary), runner)
                    run_benchmark(runner, 18, 1024 * 1024, tools,
                                  CorruptReadbackCommands())
        finally:
            runner._cleanup_mount = original_cleanup
        self.assertEqual(str(raised.exception),
                         "LTFS remount hash validation failed")
        self.assertIsNotNone(raised.exception.__cause__)
        self.assertIn("mount remains active", str(raised.exception.__cause__))

    def test_disconnected_fuse_mount_uses_lazy_cleanup(self):
        runner = load("run_file_backend_disconnected_cleanup", "run-file-backend.py")

        class DisconnectedCommands(FakeCommands):
            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if Path(normalized[0]).name.startswith("fusermount"):
                    self.commands.append(normalized)
                    if "-z" in normalized:
                        self.mounted = False
                        return subprocess.CompletedProcess(normalized, 0, "", "")
                    return subprocess.CompletedProcess(normalized, 1, "", "disconnected")
                return super().run(argv, **kwargs)

        commands = DisconnectedCommands()
        original_copy = runner.shutil.copyfileobj

        def fail_after_disconnect(*args, **kwargs):
            commands.active_process.completed = True
            raise RuntimeError("primary copy failure")

        runner.shutil.copyfileobj = fail_after_disconnect
        try:
            with tempfile.TemporaryDirectory() as temporary:
                with self.assertRaisesRegex(RuntimeError, "primary copy failure"):
                    tools = self.make_tools(Path(temporary), runner)
                    run_benchmark(runner, 11, 1024 * 1024, tools, commands)
        finally:
            runner.shutil.copyfileobj = original_copy
        self.assertFalse(commands.mounted)
        unmounts = [command for command in commands.commands
                    if Path(command[0]).name.startswith("fusermount")]
        self.assertEqual([command[1:-1] for command in unmounts],
                         [["-u"], ["-u", "-z"]])
        self.assertEqual(commands.processes[0].wait_timeouts, [30])

    def test_cleanup_error_does_not_mask_the_primary_error(self):
        runner = load("run_file_backend_cleanup_precedence", "run-file-backend.py")

        class FailingCleanupCommands(FakeCommands):
            def run(self, argv, **kwargs):
                normalized = [str(value) for value in argv]
                if Path(normalized[0]).name.startswith("fusermount"):
                    self.commands.append(normalized)
                    raise OSError("cleanup command failed")
                return super().run(argv, **kwargs)

        commands = FailingCleanupCommands()
        original_copy = runner.shutil.copyfileobj

        def fail_after_disconnect(*args, **kwargs):
            commands.active_process.completed = True
            raise RuntimeError("primary copy failure")

        runner.shutil.copyfileobj = fail_after_disconnect
        try:
            with tempfile.TemporaryDirectory() as temporary:
                with self.assertRaisesRegex(
                        RuntimeError, "primary copy failure") as raised:
                    tools = self.make_tools(Path(temporary), runner)
                    run_benchmark(runner, 12, 1024 * 1024, tools, commands)
        finally:
            runner.shutil.copyfileobj = original_copy
        self.assertIsNotNone(raised.exception.__cause__)
        self.assertIn("mount remains active", str(raised.exception.__cause__))
        unmounts = [command for command in commands.commands
                    if Path(command[0]).name.startswith("fusermount")]
        self.assertEqual([command[1:-1] for command in unmounts],
                         [["-u"], ["-u", "-z"], ["-u", "-z"]])
        self.assertEqual(commands.processes[0].wait_timeouts, [30])

    def test_prerequisites_and_dirty_tree_fail_closed(self):
        runner = load("run_file_backend_missing", "run-file-backend.py")
        commands = FakeCommands()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            missing = root / "missing"
            tools = runner.Toolchain(
                missing, missing, missing, missing, missing, missing, missing
            )
            with self.assertRaises(RuntimeError):
                run_benchmark(runner, 1, 1024, tools, commands)
            tools = self.make_tools(root, runner)
            original = commands.run
            def dirty(argv, **kwargs):
                if [str(v) for v in argv][:3] == ["git", "status", "--porcelain"]:
                    return subprocess.CompletedProcess(argv, 0, " M source.c\n", "")
                return original(argv, **kwargs)
            commands.run = dirty
            with self.assertRaises(RuntimeError):
                run_benchmark(runner, 1, 1024, tools, commands)

    def test_schema_and_comparator_reject_forged_integrity(self):
        compare = load("compare_results", "compare-results.py")
        runner = load("run_file_backend_compare", "run-file-backend.py")
        commands = FakeCommands()
        with tempfile.TemporaryDirectory() as temporary:
            tools = self.make_tools(Path(temporary), runner)
            baseline = run_benchmark(runner, 7, 1024 * 1024,
                                     tools, commands)
        compare.validate_result(baseline)
        def forge_receipt(data):
            item = data["validation"]["receipts"][0]
            receipt = json.loads(base64.b64decode(item["content_base64"]))
            receipt["files"] += 1
            raw = (json.dumps(receipt, separators=(",", ":")) + "\n").encode()
            item["content_base64"] = base64.b64encode(raw).decode()
            item["sha256"] = hashlib.sha256(raw).hexdigest()
        def forge_readonly_generation(data):
            item = data["validation"]["receipts"][1]
            receipt = json.loads(base64.b64decode(item["content_base64"]))
            receipt["new_generation"] = receipt["prior_generation"] + 1
            raw = (json.dumps(receipt, separators=(",", ":")) + "\n").encode()
            item["content_base64"] = base64.b64encode(raw).decode()
            item["sha256"] = hashlib.sha256(raw).hexdigest()
        for mutate in (
            lambda d: d["build"].__setitem__("dirty", True),
            lambda d: d["build"]["artifacts"].__setitem__("ltfs", "0" * 63),
            lambda d: d["build"]["artifacts"].__setitem__(
                "iosched_backend", "0" * 63
            ),
            lambda d: d["validation"].__setitem__("all_hashes_match", False),
            lambda d: d["validation"]["files"][0].__setitem__("matches", False),
            lambda d: d["validation"]["files"][0].__setitem__("observed_sha256", "0" * 64),
            lambda d: d["backend"].__setitem__("physical_tape", True),
            lambda d: d["validation"]["receipts"][0].__setitem__("sha256", "0" * 64),
            lambda d: d["validation"]["receipts"][0].__setitem__("content_base64", "e30="),
            forge_receipt,
            forge_readonly_generation,
        ):
            forged = json.loads(json.dumps(baseline)); mutate(forged)
            with self.assertRaises(ValueError): compare.validate_result(forged)
        rate = baseline["metrics"]["throughput_bytes_per_second"]
        threshold = (rate * 95 + 99) // 100
        self.assertFalse(compare.more_than_five_percent_slower(rate, threshold))
        self.assertTrue(compare.more_than_five_percent_slower(rate, threshold - 1))


if __name__ == "__main__": unittest.main()
