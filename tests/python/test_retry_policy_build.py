# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]


class RetryPolicyBuildTests(unittest.TestCase):
    def test_sg_close_reports_success_after_best_effort_cleanup(self):
        source = (
            ROOT / "src/tape_drivers/linux/sg/sg_tape.c"
        ).read_text(encoding="utf-8")
        start = source.index("int sg_close(void *device)")
        end = source.index("\nint sg_close_raw(void *device)", start)
        close_body = source[start:end]
        self.assertIn("int ret = 0;", close_body)
        self.assertNotIn("-EDEV_UNKNOWN", close_body)

    def test_retry_target_declares_deterministic_clock_provider(self):
        makefile = (ROOT / "tests/Makefile.am").read_text(encoding="utf-8")
        logical_lines = makefile.replace("\\\n", " ")
        source_line = next(
            line
            for line in logical_lines.splitlines()
            if line.startswith("test_retry_policy_SOURCES =")
        )
        self.assertIn("c/test_retry_clock.c", source_line.split())

    def test_generated_retry_link_contains_clock_provider_object(self):
        generated = ROOT / "tests/Makefile.in"
        self.assertTrue(generated.is_file(), "run autogen.sh before this oracle")
        makefile = generated.read_text(encoding="utf-8")
        logical_lines = makefile.replace("\\\n", " ")
        object_line = next(
            line
            for line in logical_lines.splitlines()
            if line.startswith("am_test_retry_policy_OBJECTS =")
        )
        self.assertIn(
            "c/test_retry_policy-test_retry_clock.$(OBJEXT)", object_line.split()
        )
        self.assertIn("c/test_retry_clock.c", makefile)
        self.assertIn(
            "test_retry_policy$(EXEEXT): $(test_retry_policy_OBJECTS)", makefile
        )
        self.assertIn(
            "$(LINK) $(test_retry_policy_OBJECTS) $(test_retry_policy_LDADD)",
            makefile,
        )


if __name__ == "__main__":
    unittest.main()
