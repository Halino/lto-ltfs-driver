"""Reject private operational evidence in the driver source distribution."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PRIVATE_EVIDENCE = re.compile(
    r"\bIR\d{4}\b|\bjob-[a-z0-9]+-[a-f0-9]{8}\b|/var/tmp/lto-[^\s`]+"
)


class PublicSourcePrivacyTests(unittest.TestCase):
    def test_distributed_qualification_docs_have_no_private_evidence(self):
        makefile = (ROOT / "Makefile.am").read_text(encoding="utf-8")
        documents = sorted((ROOT / "docs/qualification").glob("*.md"))
        self.assertTrue(documents)
        for document in documents:
            with self.subTest(document=document.name):
                self.assertIn("docs/qualification/" + document.name, makefile)
                self.assertIsNone(
                    PRIVATE_EVIDENCE.search(document.read_text(encoding="utf-8")),
                    f"private operational evidence in {document.name}",
                )

    def test_internal_plans_are_not_distributed(self):
        makefile = (ROOT / "Makefile.am").read_text(encoding="utf-8")
        self.assertNotIn("docs/superpowers/", makefile)
        self.assertFalse(
            list((ROOT / "docs/superpowers").rglob("*.md")),
            "internal development plans must not enter the public source archive",
        )


if __name__ == "__main__":
    unittest.main()
