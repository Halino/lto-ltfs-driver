"""Driver publication must bind one exact, fresh immutable draft proof."""

from __future__ import annotations

import runpy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch


TOOL = Path(__file__).resolve().parents[2] / "scripts/verify-public-release.py"
GUIDE = Path(__file__).resolve().parents[2] / "docs/public-release-verification.md"
REPO = "example/lto-ltfs"
COMMIT = "a" * 40
FPR = "B" * 40
SUBFPR = "C" * 40


def candidate_fixture(root: Path, names: frozenset[str]) -> dict[str, str]:
    for name in names - {"FINAL-RPM-SHA256SUMS"}:
        (root / name).write_bytes(name.encode("ascii"))
    rpm_names = sorted(name for name in names if name.endswith(".rpm"))
    (root / "FINAL-RPM-SHA256SUMS").write_text("".join(
        f"{hashlib.sha256((root / name).read_bytes()).hexdigest()}  {name}\n"
        for name in rpm_names
    ), encoding="ascii")
    return {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest()
        for name in names
    }


class PublicDriverReleaseProofTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.gate = runpy.run_path(str(TOOL))

    def test_driver_proof_rejects_wrong_draft_asset_and_age(self) -> None:
        names = self.gate["RELEASE_ASSET_NAMES"]
        assets = {name: "b" * 64 for name in names}
        digest = self.gate["asset_set_sha256"](assets)
        make = self.gate["make_final_proof"]
        validate = self.gate["validate_final_proof"]
        error = self.gate["PublicDriverReleaseError"]
        proof = make(REPO, "v0.1.1", COMMIT, 17, "c" * 64, digest, 49, 1000)
        expected = {key: value for key, value in proof.items() if key != "checked_at"}
        validate(proof, expected, 1120)
        for changed, now in (
            ({"repo": "elsewhere/lto-ltfs"}, 1000),
            ({"draft_id": 18}, 1000),
            ({"asset_set_sha256": "d" * 64}, 1000),
            ({}, 999),
            ({}, 1121),
        ):
            with self.subTest(changed=changed, now=now), self.assertRaises(error):
                validate(proof, expected | changed, now)
        changed_assets = assets | {next(iter(names)): "e" * 64}
        self.assertNotEqual(digest, self.gate["asset_set_sha256"](changed_assets))
        with self.assertRaises(error):
            self.gate["asset_set_sha256"]({name: "b" * 64 for name in names if name != next(iter(names))})

    def test_driver_immutability_404_and_disabled_refuse_before_publish(self) -> None:
        check = self.gate["verify_immutability_response"]
        error = self.gate["PublicDriverReleaseError"]
        good = b'HTTP/2.0 200 OK\r\ncontent-type: application/json\r\n\r\n{"enabled":true}'
        check(good)
        for bad in (
            good.replace(b"200 OK", b"404 Not Found"),
            good.replace(b"true", b"false"),
            good.replace(b'{"enabled":true}', b"{}"),
            good.replace(b'{"enabled":true}', b"broken"),
        ):
            with self.subTest(bad=bad[:60]), self.assertRaises(error):
                check(bad)

    def test_exact_signed_driver_assets_reject_extra_missing_and_changed_bytes(self) -> None:
        names = self.gate["RELEASE_ASSET_NAMES"]
        error = self.gate["PublicDriverReleaseError"]
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            approved = candidate_fixture(root, names)
            rpm_names = sorted(name for name in names if name.endswith(".rpm"))
            check = self.gate["verify_release_files"]
            check(root, approved)
            (root / "extra.rpm").write_bytes(b"extra")
            with self.assertRaises(error):
                check(root, approved)
            (root / "extra.rpm").unlink()
            (root / rpm_names[0]).write_bytes(b"substituted")
            with self.assertRaises(error):
                check(root, approved)
            (root / rpm_names[0]).unlink()
            with self.assertRaises(error):
                check(root, approved)

    def test_driver_attestation_command_binds_repository_workflow_and_tag(self) -> None:
        command = self.gate["attestation_command"](
            Path("driver.rpm"), REPO,
            f"{REPO}/.github/workflows/build-release.yml",
            "refs/tags/v0.1.1", COMMIT, Path("ATTESTATION.json"),
        )
        self.assertEqual(command[:3], ["gh", "attestation", "verify"])
        self.assertIn("--deny-self-hosted-runners", command)
        self.assertIn(COMMIT, command)
        for wrong_repo, wrong_signer, wrong_ref in (
            ("wrong/lto", f"{REPO}/.github/workflows/build-release.yml", "refs/tags/v0.1.1"),
            (REPO, "elsewhere/workflow.yml", "refs/tags/v0.1.1"),
            (REPO, f"{REPO}/.github/workflows/build-release.yml", "refs/heads/main"),
        ):
            with self.assertRaises(self.gate["PublicDriverReleaseError"]):
                self.gate["attestation_command"](
                    Path("driver.rpm"), wrong_repo, wrong_signer, wrong_ref,
                    COMMIT, Path("ATTESTATION.json"),
                )

    def test_full_key_and_signature_identity_not_just_short_key_id(self) -> None:
        error = self.gate["PublicDriverReleaseError"]
        listing = (
            f"pub:-:4096:1:BBBBBBBBBBBBBBBB::::::\nfpr:::::::::{FPR}:\n"
            f"sub:-:4096:1:CCCCCCCCCCCCCCCC::::::\nfpr:::::::::{SUBFPR}:\n"
        ).encode()
        self.gate["verify_public_key_records"](listing, FPR, SUBFPR)
        with self.assertRaises(error):
            self.gate["verify_public_key_records"](
                listing.replace(FPR.encode(), ("A" * 32 + "B" * 8).encode()),
                FPR, SUBFPR,
            )
        self.gate["verify_signature_output"](
            b"Header V4 RSA/SHA256 Signature, key ID cccccccc: OK\n", SUBFPR
        )
        for output in (
            b"Header V4 RSA/SHA256 Signature, key ID bbbbbbbb: OK\n",
            b"Header V4 RSA/SHA256 Signature, key ID cccccccc: NOKEY\n",
        ):
            with self.assertRaises(error):
                self.gate["verify_signature_output"](output, SUBFPR)
        self.gate["verify_gpg_status"](
            f"[GNUPG:] VALIDSIG {SUBFPR}\n".encode(), SUBFPR
        )
        with self.assertRaises(error):
            self.gate["verify_gpg_status"](
                f"[GNUPG:] VALIDSIG {FPR}\n".encode(), SUBFPR
            )

    def test_release_refuses_changed_asset_before_source_or_signature_commands(self) -> None:
        names = self.gate["RELEASE_ASSET_NAMES"]
        verify = self.gate["verify_release"]
        error = self.gate["PublicDriverReleaseError"]
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            approved = candidate_fixture(root, names)
            (root / "BUILD-INPUTS.json").write_bytes(b"substituted")
            source = Mock()
            command = Mock()
            with patch.dict(verify.__globals__, {
                "verify_source_inputs": source, "_run": command,
            }):
                with self.assertRaises(error):
                    verify(root, approved, FPR, SUBFPR, REPO, "v0.1.1", COMMIT, root)
            source.assert_not_called()
            command.assert_not_called()

    def test_approval_document_has_exact_schema_and_no_duplicate_keys(self) -> None:
        error = self.gate["PublicDriverReleaseError"]
        names = self.gate["RELEASE_ASSET_NAMES"]
        document = {
            "schema_version": 1, "repo": REPO, "tag": "v0.1.1",
            "commit": COMMIT, "primary_fingerprint": FPR,
            "signing_subkey_fingerprint": SUBFPR,
            "assets": {name: "b" * 64 for name in names},
        }
        self.assertEqual(self.gate["parse_approval_document"](json.dumps(document).encode()), document)
        for bad in (
            json.dumps(document | {"extra": True}).encode(),
            json.dumps(document | {"schema_version": 2}).encode(),
            json.dumps(document).replace('"schema_version": 1', '"schema_version": 1, "schema_version": 1').encode(),
        ):
            with self.assertRaises(error):
                self.gate["parse_approval_document"](bad)

    def test_cli_reports_approval_refusal_without_traceback_or_report(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            approval = root / "approval.json"
            approval.write_text("{}", encoding="utf-8")
            report = root / "report.json"
            with patch("sys.stderr") as stderr:
                code = self.gate["main"]([
                    "--candidate", str(root), "--approved-json", str(approval),
                    "--source-root", str(root), "--report", str(report),
                ])
            self.assertEqual(code, 2)
            self.assertFalse(report.exists())
            self.assertIn(
                "signed driver release refused",
                "".join(call.args[0] for call in stderr.write.call_args_list),
            )

    def test_draft_identity_rejects_substituted_assets_and_wrong_numeric_id(self) -> None:
        names = self.gate["RELEASE_ASSET_NAMES"]
        error = self.gate["PublicDriverReleaseError"]
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            approved = candidate_fixture(root, names)
            release = {
                "id": 82, "draft": True, "tag_name": "v0.1.1",
                "assets": [
                    {"name": name, "size": (root / name).stat().st_size, "state": "uploaded"}
                    for name in sorted(names)
                ],
            }
            check = self.gate["verify_draft_identity"]
            check(release, root, approved, 82, "v0.1.1")
            for altered in (
                release | {"id": 83},
                release | {"draft": False},
                release | {"assets": release["assets"][:-1]},
            ):
                with self.assertRaises(error):
                    check(altered, root, approved, 82, "v0.1.1")
            (root / next(iter(names))).write_bytes(b"changed")
            with self.assertRaises(error):
                check(release, root, approved, 82, "v0.1.1")

    def test_smoke_report_must_bind_run_commit_and_pinned_image(self) -> None:
        report = {
            "schema_version": 1, "status": "passed_no_tape_install_fixture_remove",
            "repo": REPO, "commit": COMMIT, "run_id": 49,
            "image": self.gate["PINNED_UBI_IMAGE"],
            "unsigned_binary_sha256": "a" * 64,
        }
        check = self.gate["verify_smoke_report"]
        check(report, REPO, COMMIT, 49)
        for changed in ({"run_id": 50}, {"status": "failed"}, {"image": "latest"}):
            with self.assertRaises(self.gate["PublicDriverReleaseError"]):
                check(report | changed, REPO, COMMIT, 49)

    def test_operator_guide_discloses_exact_gates_and_limitations(self) -> None:
        guide = " ".join(GUIDE.read_text(encoding="utf-8").split())
        for required in (
            "separate source/tag approval", "separate final-asset approval",
            "conditional and unverified", "LGPL-2.1-only", "no physical tape",
            "no DNF repository", "non-atomic", "verify-public-release.py",
            "rpm -K", "gh attestation verify", "SOURCE-MANIFEST.json",
        ):
            self.assertIn(required, guide)


if __name__ == "__main__":
    unittest.main()
