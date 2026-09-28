# SPDX-License-Identifier: BSD-3-Clause

import os
import pathlib
import shlex
import subprocess
import tempfile
import unittest


def compile_installed_header(
    include_root, consumer, environment=None, runner=subprocess.run
):
    environment = os.environ if environment is None else environment
    c_compiler = shlex.split(environment.get("CC", "cc"))
    cxx_compiler = shlex.split(environment.get("CXX", "c++"))
    common = [
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsyntax-only",
        f"-I{include_root}",
        str(consumer),
    ]
    runner([*c_compiler, "-std=c11", *common], check=True)
    runner([*cxx_compiler, "-std=c++11", *common], check=True)


class FinalizationInstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = pathlib.Path(
            os.environ.get("LTFS_SOURCE_DIR", pathlib.Path(__file__).parents[2])
        ).resolve()
        cls.build = pathlib.Path(
            os.environ.get("LTFS_BUILD_DIR", cls.source)
        ).resolve()

    def test_header_is_in_install_manifest(self):
        manifest = (self.source / "src" / "Makefile.am").read_text(encoding="utf-8")
        installed = manifest.split("nobase_pkginclude_HEADERS =", 1)[1]
        self.assertIn("libltfs/finalization.h", installed)

    def test_consumer_compile_uses_cc_and_cxx_independently(self):
        commands = []

        def record(command, check):
            self.assertTrue(check)
            commands.append(command)

        compile_installed_header(
            pathlib.Path("/installed/include"),
            pathlib.Path("/tmp/consumer.c"),
            environment={"CC": "gcc -m64", "CXX": "g++ -m64"},
            runner=record,
        )
        self.assertEqual(commands[0][:3], ["gcc", "-m64", "-std=c11"])
        self.assertEqual(commands[1][:3], ["g++", "-m64", "-std=c++11"])
        for command in commands:
            self.assertIn("-Wall", command)
            self.assertIn("-Wextra", command)
            self.assertIn("-Werror", command)
            self.assertIn("-fsyntax-only", command)
            self.assertIn("-I/installed/include", command)

        commands.clear()
        compile_installed_header(
            pathlib.Path("/installed/include"),
            pathlib.Path("/tmp/consumer.c"),
            environment={},
            runner=record,
        )
        self.assertEqual(commands[0][0], "cc")
        self.assertEqual(commands[1][0], "c++")

    def test_installed_header_compiles_as_c_and_cxx(self):
        makefile = self.build / "src" / "Makefile"
        if not makefile.exists():
            self.skipTest("configured build tree required for install consumer test")
        with tempfile.TemporaryDirectory() as directory:
            destination = pathlib.Path(directory)
            subprocess.run(
                [
                    "make",
                    "-C",
                    str(self.build / "src"),
                    "install-nobase_pkgincludeHEADERS",
                    f"DESTDIR={destination}",
                ],
                check=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            headers = list(destination.glob("**/ltfs/libltfs/finalization.h"))
            self.assertEqual(len(headers), 1)
            include_root = headers[0].parents[2]
            consumer = destination / "consumer.c"
            consumer.write_text(
                "#include <ltfs/libltfs/finalization.h>\n"
                "static struct ltfs_commit_receipt receipt;\n"
                "int main(void) { return receipt.result; }\n",
                encoding="utf-8",
            )
            compile_installed_header(include_root, consumer)


if __name__ == "__main__":
    unittest.main()
