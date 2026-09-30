"""No public driver PR or build job may reach protected signing/publication."""

from __future__ import annotations

import re
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
CI = ROOT / ".github/workflows/ci.yml"
BUILD = ROOT / ".github/workflows/build-release.yml"
SIGNER = ROOT / "scripts/sign-public-rpm.sh"
PUBLISH = ROOT / ".github/workflows/publish-release.yml"


def _read_workflow(path: Path) -> tuple[str, dict]:
    text = path.read_text(encoding="utf-8")
    return text, yaml.safe_load(text)


class PublicWorkflowTests(unittest.TestCase):
    def test_ci_provenance_fixture_checks_real_git_identity_outside_source(self):
        _text, data = _read_workflow(CI)
        steps = data['jobs']['source-checks']['steps']
        fixture = next((s for s in steps if s.get('name') == 'Prepare pinned upstream provenance fixture'), None)
        self.assertIsNotNone(fixture, 'CI must prepare the full upstream oracle')
        with tempfile.TemporaryDirectory() as raw:
            base = Path(raw)
            upstream = base / 'origin'
            upstream.mkdir()
            def git(*args):
                return subprocess.run(['git', '-C', str(upstream), *args], check=True, capture_output=True, text=True).stdout.strip()
            git('init', '-q')
            git('config', 'user.email', 'fixture@example.test')
            git('config', 'user.name', 'Fixture')
            (upstream / 'LICENSE').write_text('controlled upstream fixture\n')
            git('add', '.')
            git('commit', '-qm', 'Fixture')
            git('tag', 'v2.4.8.4-10522')
            commit, tree = git('rev-parse', 'HEAD'), git('rev-parse', 'HEAD^{tree}')
            script = fixture['run'].replace('https://github.com/LinearTapeFileSystem/ltfs.git', str(upstream))
            script = script.replace('7d0de7c0a71296353160f4c5bc082fec9af04e5c', commit).replace('5e22b0e576d2cb15deb41bdee1073f862c5b48e0', tree)
            env = dict(os.environ, RUNNER_TEMP=str(base / 'runner'), GITHUB_ENV=str(base / 'env'))
            Path(env['RUNNER_TEMP']).mkdir()
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script], cwd=ROOT, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            path = Path(Path(env['GITHUB_ENV']).read_text().strip().split('=', 1)[1])
            self.assertFalse(path.is_relative_to(ROOT))
            self.assertEqual(subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip(), commit)
            # A moved upstream tag must fail before the fixture is exported.
            (upstream / 'LICENSE').write_text('changed upstream\n')
            git('commit', '-qam', 'Changed')
            git('tag', '-f', 'v2.4.8.4-10522')
            env['RUNNER_TEMP'] = str(base / 'runner-bad')
            env['GITHUB_ENV'] = str(base / 'bad-env')
            Path(env['RUNNER_TEMP']).mkdir()
            result = subprocess.run(['bash', '-euo', 'pipefail', '-c', script], cwd=ROOT, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(Path(env['GITHUB_ENV']).exists())

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
        self.assertIn("driver-no-tape-smoke-report", str(smoke))
        self.assertIn("driver-smoke-report.json", str(smoke))

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
        self.assertNotIn("--compare-first incoming/unsigned", str(sign))

    def test_signer_pins_both_rpms_and_checks_post_signatures(self) -> None:
        script = SIGNER.read_text(encoding="utf-8")
        self.assertIn("lto-ltfs-0.1.1-22.el9.x86_64.rpm", script)
        self.assertIn("lto-ltfs-0.1.1-22.el9.src.rpm", script)
        self.assertIn("--compare-first", script)
        self.assertIn("--addsign", script)
        self.assertIn("rpmkeys", script)
        self.assertIn("-K", script)
        self.assertIn("FINAL-RPM-SHA256SUMS.asc", script)
        self.assertNotIn("gh release", script)

    def test_driver_finalizer_has_no_second_approval_or_admin_token(self) -> None:
        text, data = _read_workflow(PUBLISH)
        self.assertEqual(set(data["jobs"]), {
            "preflight", "publish", "final_preflight", "finalize",
        })
        self.assertEqual(data["permissions"], {"contents": "read"})
        self.assertEqual(data["jobs"]["preflight"]["environment"], "public-driver-rpm-admin-read")
        self.assertEqual(data["jobs"]["final_preflight"]["environment"], "public-driver-rpm-admin-read")
        self.assertEqual(data["jobs"]["publish"]["environment"], "public-driver-rpm-publication")
        self.assertNotIn("environment", data["jobs"]["finalize"])
        self.assertEqual(data["jobs"]["finalize"]["needs"], "final_preflight")
        for name in ("publish", "finalize"):
            self.assertEqual(data["jobs"][name]["permissions"]["contents"], "write")
            self.assertNotIn("PUBLIC_DRIVER_ADMIN_READ_TOKEN", str(data["jobs"][name]))
            self.assertNotIn("PUBLIC_DRIVER_RPM_SECRET_KEY_B64", str(data["jobs"][name]))
        for name in ("preflight", "final_preflight"):
            self.assertEqual(data["jobs"][name]["permissions"]["contents"], "read")
            self.assertNotIn("gh release create", str(data["jobs"][name]))
            self.assertNotIn("gh release edit", str(data["jobs"][name]))
        self.assertIn("--verify-immutability-http", str(data["jobs"]["preflight"]))
        self.assertIn("--verify-immutability-http", str(data["jobs"]["final_preflight"]))
        self.assertIn("validate_final_proof", str(data["jobs"]["finalize"]))
        self.assertIn("gh release edit", str(data["jobs"]["finalize"]))
        self.assertIn("isImmutable", str(data["jobs"]["finalize"]))
        for job in data["jobs"].values():
            self.assertEqual(job["if"], "startsWith(github.ref, 'refs/tags/v')")
        preflight_steps = data["jobs"]["preflight"]["steps"]
        self.assertIn("refs/tags/$RELEASE_TAG", preflight_steps[1]["run"])
        self.assertNotIn("secrets.", str(preflight_steps[1]))
        self.assertIn("PUBLIC_DRIVER_ADMIN_READ_TOKEN", str(preflight_steps[2]))
        self._assert_pinned_actions(text)

    def _assert_pinned_actions(self, text: str) -> None:
        actions = re.findall(r"(?m)^\s*-?\s*uses:\s*([^\s#]+)", text)
        self.assertTrue(actions)
        for action in actions:
            self.assertRegex(action, r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+@[0-9a-f]{40}$")


if __name__ == "__main__":
    unittest.main()
