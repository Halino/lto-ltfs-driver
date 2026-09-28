# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
DEFINITION = re.compile(
    r"^(?P<static>static\s+)?volatile\s+char\s+\*"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*=\s*LTFS_COPYRIGHT_0",
    re.MULTILINE,
)


class CopyrightSymbolTests(unittest.TestCase):
    def test_embedded_copyright_symbols_have_unique_external_linkage(self):
        definitions = []
        sources = []
        for source in sorted((ROOT / "src").rglob("*.c")):
            text = source.read_text(encoding="utf-8")
            if "LTFS_COPYRIGHT_0" not in text:
                continue
            sources.append(source.relative_to(ROOT).as_posix())
            matches = list(DEFINITION.finditer(text))
            self.assertEqual(
                len(matches),
                1,
                f"{source.relative_to(ROOT)} must retain one embedded copyright symbol",
            )
            match = matches[0]
            self.assertIsNone(
                match.group("static"),
                f"{source.relative_to(ROOT)} copyright string must remain in linked output",
            )
            definitions.append((match.group("name"), source.relative_to(ROOT).as_posix()))

        self.assertTrue(sources, "no embedded copyright definitions found")
        names = [name for name, _ in definitions]
        self.assertNotIn("copyright", names)
        self.assertEqual(
            len(names),
            len(set(names)),
            f"duplicate external copyright symbols: {definitions}",
        )


if __name__ == "__main__":
    unittest.main()
