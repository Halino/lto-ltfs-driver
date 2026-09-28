# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import re
import shlex
import unittest


ROOT = Path(__file__).resolve().parents[2]
BUILD_PACKAGES = {"autoconf", "automake", "libtool", "gcc", "make"}
SHELL_SEPARATORS = {"&&", "||", ";", "|"}


def containerfile_instructions(containerfile):
    logical_lines = []
    continued = []

    for line in containerfile.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            if continued and stripped.startswith("#"):
                raise ValueError("comment inside a continued instruction")
            if not continued:
                continue
        continued.append(stripped.rstrip("\\").rstrip())
        if line.rstrip().endswith("\\"):
            continue
        logical_lines.append(" ".join(continued))
        continued = []
    if continued:
        logical_lines.append(" ".join(continued))

    for logical_line in logical_lines:
        keyword, separator, payload = logical_line.partition(" ")
        if separator:
            yield keyword.upper(), payload


def installed_package_blocks(run_payload):
    if "#" in run_payload:
        raise ValueError("comments are not allowed in build RUN instructions")
    lexer = shlex.shlex(run_payload, posix=True, punctuation_chars=";&|")
    lexer.whitespace_split = True
    lexer.commenters = "#"

    commands = []
    command = []
    for token in lexer:
        if token in SHELL_SEPARATORS:
            if command:
                commands.append(command)
                command = []
            continue
        if any(character in token for character in ";&|"):
            raise ValueError("unsupported shell separator")
        command.append(token)
    if command:
        commands.append(command)

    blocks = []
    for command in commands:
        if command[:2] != ["dnf", "install"]:
            continue
        packages = []
        for token in command[2:]:
            if token == "-y" or token.startswith("--"):
                continue
            packages.append(token)
        blocks.append(packages)
    return blocks


def installed_packages(containerfile):
    packages = set()
    for keyword, payload in containerfile_instructions(containerfile):
        if keyword != "RUN":
            continue
        for block in installed_package_blocks(payload):
            packages.update(block)
    return packages


def cxx_build_dependency_is_valid(containerfile):
    stage = -1
    instruction_index = -1
    build_dependency_blocks = []
    cxx_locations = []
    first_build_by_stage = {}

    try:
        for keyword, payload in containerfile_instructions(containerfile):
            instruction_index += 1
            if keyword == "FROM":
                stage += 1
                continue
            if keyword != "RUN" or stage < 0:
                continue
            blocks = installed_package_blocks(payload)
            for block_index, block in enumerate(blocks):
                location = (stage, instruction_index, block_index)
                if BUILD_PACKAGES.issubset(block):
                    build_dependency_blocks.append(location)
                cxx_locations.extend(
                    location for package in block if package == "gcc-c++"
                )
            if (
                "./autogen.sh" in payload
                or "./configure" in payload
                or re.search(r"(?:^|&&)\s*make(?:\s|$)", payload)
            ):
                first_build_by_stage.setdefault(stage, instruction_index)
    except ValueError:
        return False

    if len(build_dependency_blocks) != 1 or len(cxx_locations) != 1:
        return False
    dependency_location = build_dependency_blocks[0]
    cxx_location = cxx_locations[0]
    build_index = first_build_by_stage.get(dependency_location[0])
    return (
        cxx_location == dependency_location
        and build_index is not None
        and dependency_location[1] < build_index
    )


class ContainerfileTests(unittest.TestCase):
    def test_cxx_frontend_is_an_explicit_build_dependency(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        self.assertTrue(cxx_build_dependency_is_valid(containerfile))

    def test_cxx_frontend_cannot_be_absent(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        mutated = containerfile.replace("        gcc-c++ \\\n", "", 1)
        self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_cxx_frontend_cannot_be_duplicated_in_same_install(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        mutated = containerfile.replace(
            "        gcc-c++ \\\n",
            "        gcc-c++ \\\n        gcc-c++ \\\n",
            1,
        )
        self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_cxx_frontend_cannot_be_duplicated_in_another_install_block(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        mutated = containerfile.replace(
            "        fuse-devel \\\n",
            "        fuse-devel \\\n        gcc-c++ \\\n",
            1,
        )
        self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_cxx_frontend_cannot_be_moved_after_build(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        mutated = containerfile.replace("        gcc-c++ \\\n", "", 1)
        mutated += "\nRUN dnf install -y gcc-c++\n"
        self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_cxx_frontend_cannot_be_moved_to_later_stage(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        mutated = containerfile.replace("        gcc-c++ \\\n", "", 1)
        mutated += (
            "\nFROM registry.access.redhat.com/ubi9/ubi AS late\n"
            "RUN dnf install -y gcc-c++\n"
        )
        self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_cxx_frontend_in_continued_comment_is_rejected(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        mutated = containerfile.replace(
            "        gcc-c++ \\\n",
            "        # gcc-c++ \\\n",
            1,
        )
        self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_cxx_frontend_after_shell_separator_is_not_a_package(self):
        containerfile = (ROOT / "Containerfile.build").read_text()
        for separator in (";", "||", "|"):
            with self.subTest(separator=separator):
                mutated = containerfile.replace("        gcc-c++ \\\n", "", 1)
                mutated = mutated.replace(
                    "        diffutils \\\n",
                    f"        diffutils {separator} true gcc-c++ \\\n",
                    1,
                )
                self.assertFalse(cxx_build_dependency_is_valid(mutated))

    def test_genrb_provider_is_an_explicit_build_dependency(self):
        packages = installed_packages((ROOT / "Containerfile.build").read_text())
        self.assertIn("icu", packages)

    def test_sanitizer_runtimes_are_explicit_build_dependencies(self):
        packages = installed_packages((ROOT / "Containerfile.build").read_text())
        self.assertTrue({"libasan", "libubsan"}.issubset(packages))


if __name__ == "__main__":
    unittest.main()
