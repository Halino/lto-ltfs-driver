"""Closed public dependency inputs for the driver GitHub build."""

from __future__ import annotations

import hashlib
import http.server
import json
import runpy
import stat
import tempfile
import threading
import unittest
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FETCHER = ROOT / "scripts/fetch-public-rpms.py"


def fixture_lock(build: bytes = b"build-rpm", runtime: bytes = b"runtime-rpm") -> dict:
    repository = {
        "id": "ubi-baseos",
        "base_url": "https://cdn.example.test/ubi9/baseos/",
    }
    packages = []
    for role, name, data in (
        ("build", "libicu", build),
        ("runtime", "fuse-libs", runtime),
    ):
        filename = f"{name}-1.0-1.el9.x86_64.rpm"
        packages.append({
            "role": role,
            "repository": "ubi-baseos",
            "url": repository["base_url"] + filename,
            "filename": filename,
            "name": name,
            "epoch": "0",
            "version": "1.0",
            "release": "1.el9",
            "arch": "x86_64",
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
            "signer_fingerprint": "B" * 40,
        })
    return {
        "schema": 1,
        "image": "registry.access.redhat.com/ubi9/ubi@sha256:"
        "5426a8f45e80a07168a30ea24d84f266094b3756624a5508cc53927e6ee39e09",
        "gpg_key_sha256": "a" * 64,
        "gpg_fingerprints": ["B" * 40],
        "repositories": [repository],
        "packages": packages,
    }


class PublicDependenciesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.fetcher = runpy.run_path(str(FETCHER))

    def test_valid_fixture_and_non_https_rejection(self) -> None:
        check = self.fetcher["validate_lock"]
        error = self.fetcher["DependencyError"]
        lock = fixture_lock()
        check(lock)
        lock["packages"][0]["url"] = lock["packages"][0]["url"].replace(
            "https://", "http://"
        )
        with self.assertRaises(error):
            check(lock)

    def test_duplicate_package_or_wrong_architecture_is_rejected(self) -> None:
        check = self.fetcher["validate_lock"]
        error = self.fetcher["DependencyError"]
        lock = fixture_lock()
        lock["packages"].append(dict(lock["packages"][0]))
        with self.assertRaises(error):
            check(lock)
        lock = fixture_lock()
        lock["packages"][0]["arch"] = "s390x"
        with self.assertRaises(error):
            check(lock)

    def test_bundle_rejects_changed_and_extra_rpm(self) -> None:
        check = self.fetcher["verify_bundle"]
        error = self.fetcher["DependencyError"]
        lock = fixture_lock()
        with tempfile.TemporaryDirectory() as raw:
            bundle = Path(raw)
            for package, data in zip(lock["packages"], (b"build-rpm", b"runtime-rpm")):
                target = bundle / package["role"] / package["filename"]
                target.parent.mkdir(exist_ok=True)
                target.write_bytes(data)
            check(lock, bundle)
            target.write_bytes(b"changed")
            with self.assertRaises(error):
                check(lock, bundle)
            target.write_bytes(b"runtime-rpm")
            (bundle / "runtime/extra.rpm").write_bytes(b"extra")
            with self.assertRaises(error):
                check(lock, bundle)

    def test_committed_lock_names_exact_pinned_public_transaction(self) -> None:
        lock = json.loads(
            (ROOT / "packaging/rpm/public-dependencies.json").read_text(encoding="utf-8")
        )
        self.fetcher["validate_lock"](lock)
        self.assertEqual(166, len(lock["packages"]))
        self.assertEqual(
            "registry.access.redhat.com/ubi9/ubi@sha256:"
            "5426a8f45e80a07168a30ea24d84f266094b3756624a5508cc53927e6ee39e09",
            lock["image"],
        )
        self.assertEqual(
            {"ubi-9-baseos-rpms", "ubi-9-appstream-rpms", "ubi-9-codeready-builder-rpms"},
            {row["id"] for row in lock["repositories"]},
        )
        self.assertTrue(all(row["url"].startswith("https://cdn-ubi.redhat.com/")
                            for row in lock["packages"]))

    def test_local_http_fixture_rejects_redirect_and_changed_bytes(self) -> None:
        download = self.fetcher["_download_verified"]
        error = self.fetcher["DependencyError"]

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", "/payload")
                    self.end_headers()
                    return
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b"build-rpm")

            def log_message(self, *_args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(thread.join)
        self.addCleanup(server.shutdown)
        base = f"http://127.0.0.1:{server.server_port}"
        with tempfile.TemporaryDirectory() as raw:
            target = Path(raw) / "package.rpm"
            row = {"url": base + "/payload", "size": 9,
                   "sha256": hashlib.sha256(b"build-rpm").hexdigest()}
            download(row, target, urllib.request.build_opener())
            self.assertEqual(b"build-rpm", target.read_bytes())
            target.unlink()
            with self.assertRaises(error):
                download({**row, "url": base + "/redirect"}, target,
                         urllib.request.build_opener())
            self.assertFalse(target.exists())
            with self.assertRaises(error):
                download({**row, "sha256": "0" * 64}, target,
                         urllib.request.build_opener())

    def test_rpm_inspector_rejects_wrong_architecture_and_signer(self) -> None:
        inspect = self.fetcher["_verify_rpm"]
        error = self.fetcher["DependencyError"]
        row = fixture_lock()["packages"][0]
        row["signer_fingerprint"] = "567E347AD0044ADE55BA8A5F199E2F91FD431D51"
        metadata = (
            "libicu|0|1.0|1.el9|x86_64|(none)|"
            "RSA/SHA256, Key ID 199e2f91fd431d51\n"
        )
        signature = "Header V4 RSA/SHA256 Signature, key ID fd431d51: OK\n"

        def runner(arguments):
            return metadata if arguments[0] == "/usr/bin/rpm" else signature

        inspect(row, Path("/tmp/fixture.rpm"), Path("/tmp/rpmdb"), runner=runner)
        with self.assertRaises(error):
            inspect({**row, "arch": "noarch"}, Path("/tmp/fixture.rpm"),
                    Path("/tmp/rpmdb"), runner=runner)
        with self.assertRaises(error):
            inspect({**row, "signer_fingerprint": "B" * 40},
                    Path("/tmp/fixture.rpm"), Path("/tmp/rpmdb"), runner=runner)

    def test_fetch_output_contains_only_readable_bundle_and_manifest(self) -> None:
        fetch = self.fetcher["fetch_public_rpms"]
        globals_for_fetch = fetch.__globals__
        originals = {
            key: globals_for_fetch[key]
            for key in ("_verify_key", "_download_verified", "_verify_rpm")
        }
        contents = {"build": b"build-rpm", "runtime": b"runtime-rpm"}
        try:
            globals_for_fetch["_verify_key"] = lambda *_args: None
            globals_for_fetch["_verify_rpm"] = lambda *_args: None
            globals_for_fetch["_download_verified"] = (
                lambda row, target: target.write_bytes(contents[row["role"]])
            )
            with tempfile.TemporaryDirectory() as raw:
                root = Path(raw)
                lock = root / "lock.json"
                lock.write_text(
                    json.dumps(fixture_lock(), sort_keys=True, indent=2) + "\n",
                    encoding="utf-8",
                )
                output = root / "fetched"
                fetch(lock, output)
                self.assertEqual(
                    {"rpm-bundle", "RPM-BUNDLE.sha256"},
                    {path.name for path in output.iterdir()},
                )
                self.assertEqual(0o755, stat.S_IMODE(output.stat().st_mode))
                self.fetcher["verify_bundle"](fixture_lock(), output / "rpm-bundle")
        finally:
            globals_for_fetch.update(originals)


if __name__ == "__main__":
    unittest.main()
