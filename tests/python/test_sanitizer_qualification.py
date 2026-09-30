# SPDX-License-Identifier: BSD-3-Clause

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "run-sanitizer-qualification.sh"


class SanitizerQualificationTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.root = Path(self.tempdir.name)
        (self.root / "scripts").mkdir()
        (self.root / "src").mkdir()
        (self.root / "bin").mkdir()
        (self.root / "Makefile").touch()
        self.log = self.root / "qualification.log"

        self._write_executable(
            self.root / "autogen.sh",
            "#!/bin/sh\nprintf 'autogen\\n' >> \"$LTFS_QUALIFICATION_LOG\"\n",
        )
        self._write_executable(
            self.root / "configure",
            "#!/bin/sh\n"
            "printf 'configure:ASAN_OPTIONS=%s:UBSAN_OPTIONS=%s:CC=%s:CFLAGS=%s:%s\\n' "
            "\"${ASAN_OPTIONS-}\" \"${UBSAN_OPTIONS-}\" \"$CC\" \"$CFLAGS\" \"$*\" "
            ">> \"$LTFS_QUALIFICATION_LOG\"\n",
        )
        self._write_executable(
            self.root / "bin" / "make",
            "#!/bin/sh\n"
            "printf 'make:ASAN_OPTIONS=%s:UBSAN_OPTIONS=%s:%s\\n' "
            "\"${ASAN_OPTIONS-}\" \"${UBSAN_OPTIONS-}\" \"$*\" "
            ">> \"$LTFS_QUALIFICATION_LOG\"\n",
        )
        self._write_executable(
            self.root / "src" / "ltfs",
            "#!/bin/sh\n"
            "printf 'ltfs:ASAN_OPTIONS=%s:UBSAN_OPTIONS=%s:%s\\n' "
            "\"${ASAN_OPTIONS-}\" \"${UBSAN_OPTIONS-}\" \"$*\" "
            ">> \"$LTFS_QUALIFICATION_LOG\"\n"
            "if test \"${LTFS_BAD_VERSION_OUTPUT:-0}\" = 1; then\n"
            "  printf 'wrong-version\\n'\n"
            "elif test \"${LTFS_NO_VERSION_NEWLINE:-0}\" = 1; then\n"
            "  printf 'lto-ltfs 0.1.1'\n"
            "else\n"
            "  printf 'lto-ltfs 0.1.1\\n'\n"
            "fi\n",
        )

    def _write_executable(self, path, contents):
        path.write_text(contents, encoding="utf-8")
        path.chmod(0o755)

    def _run(self, **extra_env):
        self.assertTrue(SCRIPT.is_file(), "sanitizer qualification script is missing")
        copied_script = self.root / "scripts" / SCRIPT.name
        shutil.copy2(SCRIPT, copied_script)
        env = os.environ.copy()
        env.update(
            {
                "PATH": f"{self.root / 'bin'}:{env['PATH']}",
                "LTFS_QUALIFICATION_LOG": str(self.log),
            }
        )
        env.update(extra_env)
        return subprocess.run(
            ["/bin/sh", str(copied_script)],
            env=env,
            text=True,
            capture_output=True,
            timeout=10,
        )

    def test_builds_before_check_with_sanitizer_compiler_and_checks_versions(self):
        run = self._run(
            ASAN_OPTIONS="detect_leaks=1:halt_on_error=0",
            UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
        )
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(
            self.log.read_text(encoding="utf-8").splitlines(),
            [
                "autogen",
                "make:ASAN_OPTIONS=detect_leaks=1:halt_on_error=0:"
                "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:distclean",
                "configure:ASAN_OPTIONS=detect_leaks=0:"
                "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:"
                "CC=gcc -fsanitize=address,undefined:"
                "CFLAGS=-O1 -g -fno-omit-frame-pointer:"
                "--enable-tests --disable-snmp --disable-lintape",
                "make:ASAN_OPTIONS=detect_leaks=0:"
                "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:-j2",
                "make:ASAN_OPTIONS=detect_leaks=0:"
                "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:check",
                "ltfs:ASAN_OPTIONS=detect_leaks=0:"
                "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:--version",
                "ltfs:ASAN_OPTIONS=detect_leaks=0:"
                "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:-V",
            ],
        )

    def test_rejects_inexact_sanitized_version_output(self):
        run = self._run(LTFS_BAD_VERSION_OUTPUT="1")
        self.assertNotEqual(run.returncode, 0)

    @unittest.skipUnless(shutil.which("gcc"), "gcc is required for UBSan probe")
    def test_undefined_behavior_fails_even_without_caller_failfast_options(self):
        # Exercise the runner's actual runtime policy, not just its env text.
        probe = self.root / "ub.c"
        probe.write_text(
            "#include <limits.h>\n"
            "int main(void) { volatile int n = INT_MAX; return (n + 1) == 0; }\n",
            encoding="utf-8",
        )
        compiler = shutil.which("gcc")
        subprocess.run(
            [compiler, "-fsanitize=undefined", str(probe), "-o", str(self.root / "ub")],
            check=True, capture_output=True, text=True, timeout=10,
        )
        self._write_executable(
            self.root / "bin" / "make",
            '#!/bin/sh\nif test "$1" = check; then exec ./ub; fi\n',
        )
        for options in ("", "halt_on_error=0"):
            with self.subTest(options=options):
                run = self._run(UBSAN_OPTIONS=options)
                self.assertNotEqual(run.returncode, 0, run.stderr)
                self.assertIn("runtime error", run.stderr)

    def test_rejects_sanitized_version_output_without_final_newline(self):
        run = self._run(LTFS_NO_VERSION_NEWLINE="1")
        self.assertNotEqual(run.returncode, 0)

    def test_qualification_runner_is_in_source_distributions(self):
        makefile = (ROOT / "Makefile.am").read_text(encoding="utf-8")
        logical_lines = makefile.replace("\\\n", " ")
        self.assertRegex(
            logical_lines,
            r"(?m)^EXTRA_DIST\s*=.*\bscripts/run-sanitizer-qualification\.sh\b",
        )


if __name__ == "__main__":
    unittest.main()
