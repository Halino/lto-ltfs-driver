"""Driver publication must bind one exact, fresh immutable draft proof."""

from __future__ import annotations

import runpy
import unittest
from pathlib import Path


TOOL = Path(__file__).resolve().parents[2] / "scripts/verify-public-release.py"
REPO = "example/lto-ltfs"
COMMIT = "a" * 40


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
        proof = make(REPO, "v0.1.0", COMMIT, 17, "c" * 64, digest, 49, 1000)
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


if __name__ == "__main__":
    unittest.main()
