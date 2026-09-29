"""No public driver PR or build job may reach protected signing/publication."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
CI = ROOT / ".github/workflows/ci.yml"
BUILD = ROOT / ".github/workflows/build-release.yml"
SIGNER = ROOT / "scripts/sign-public-rpm.sh"


def _read_workflow(path: Path) -> tuple[str, dict]:
    text = path.read_text(encoding="utf-8")
    return text, yaml.safe_load(text)


class PublicWorkflowTests(unittest.TestCase):
    def test_pr_workflow_has_no_privileged_path(self) -> None:
        text, data = _read_workflow(CI)
        self.assertIn("pull_request:", text)
        self.assertNotIn("pull_request_target:", text)
        self.assertNotIn("self-hosted", text)
        self.assertNotIn("secrets.", text)
        self.assertEqual(data["permissions"], {"contents": "read"})
        for job in data["jobs"].values():
            self.assertEqual(job["runs-on"], "ubuntu-24.04")
            self.assertNotIn("environment", job)
        self._assert_pinned_actions(text)

    def test_two_builds_independently_fetch_exact_public_inputs(self) -> None:
        text, data = _read_workflow(BUILD)
        self.assertIn("workflow_dispatch:", text)
        self.assertNotIn("pull_request_target:", text)
        self.assertNotIn("self-hosted", text)
        self.assertEqual(set(data["jobs"]), {"build-a", "build-b", "compare", "smoke", "sign"})
        for name in ("build-a", "build-b"):
            job = data["jobs"][name]
            self.assertEqual(job["runs-on"], "ubuntu-24.04")
            self.assertNotIn("needs", job)
            self.assertNotIn("environment", job)
            self.assertEqual(job["permissions"], {"contents": "read"})
            script = "\n".join(step.get("run", "") for step in job["steps"])
            for required in (
                "fetch-public-rpms.py", "prepare-icu-build-tools.py",
                "build-public-unsigned.sh", "--prepare", "RELEASE_TAG",
                "registry.access.redhat.com/ubi9/ubi@sha256:",
            ):
                self.assertIn(required, script)
            self.assertNotIn("secrets.", str(job))
        compare = data["jobs"]["compare"]
        self.assertEqual(set(compare["needs"]), {"build-a", "build-b"})
        self.assertEqual(compare["permissions"], {"contents": "read"})
        self.assertIn("--compare-first", str(compare))
        self.assertIn("--compare-second", str(compare))
        self._assert_pinned_actions(text)

    def test_runtime_smoke_is_provider_free_and_precedes_signing(self) -> None:
        _text, data = _read_workflow(BUILD)
        smoke = data["jobs"]["smoke"]
        self.assertEqual(smoke["needs"], ["compare"])
        self.assertEqual(smoke["permissions"], {"contents": "read"})
        self.assertNotIn("environment", smoke)
        script = "\n".join(step.get("run", "") for step in smoke["steps"])
        self.assertIn("ltfs-info --self-test-fixture ready", script)
        self.assertIn("rpm -V lto-ltfs", script)
        self.assertIn("rpm -e lto-ltfs", script)
        self.assertNotIn("prepare-icu-build-tools", script)
        self.assertNotIn("icu/tools", script)
        self.assertEqual(set(data["jobs"]["sign"]["needs"]), {"compare", "smoke"})

    def test_signing_is_protected_key_only_and_never_publishes(self) -> None:
        text, data = _read_workflow(BUILD)
        sign = data["jobs"]["sign"]
        self.assertEqual(set(sign["needs"]), {"compare", "smoke"})
        self.assertEqual(sign["environment"], "public-driver-rpm-signing")
        self.assertEqual(sign["permissions"], {
            "contents": "read", "id-token": "write", "attestations": "write",
        })
        self.assertIn("sign-public-rpm.sh", str(sign))
        self.assertIn("attest-build-provenance", str(sign))
        self.assertIn("secrets.PUBLIC_DRIVER_RPM_SECRET_KEY_B64", str(sign))
        for name in ("build-a", "build-b", "compare", "smoke"):
            self.assertNotIn("PUBLIC_DRIVER_RPM_SECRET_KEY_B64", str(data["jobs"][name]))
        self.assertNotIn("contents: write", text)
        self.assertNotIn("gh release create", text)

    def test_signer_pins_both_rpms_and_checks_post_signatures(self) -> None:
        script = SIGNER.read_text(encoding="utf-8")
        self.assertIn("lto-ltfs-0.1.0-22.el9.x86_64.rpm", script)
        self.assertIn("lto-ltfs-0.1.0-22.el9.src.rpm", script)
        self.assertIn("--compare-first", script)
        self.assertIn("--addsign", script)
        self.assertIn("rpmkeys", script)
        self.assertIn("-K", script)
        self.assertIn("FINAL-RPM-SHA256SUMS.asc", script)
        self.assertNotIn("gh release", script)

    def _assert_pinned_actions(self, text: str) -> None:
        actions = re.findall(r"(?m)^\s*-?\s*uses:\s*([^\s#]+)", text)
        self.assertTrue(actions)
        for action in actions:
            self.assertRegex(action, r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
