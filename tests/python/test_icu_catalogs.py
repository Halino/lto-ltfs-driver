"""UBI-only loader and deterministic real-message catalog admission."""

from __future__ import annotations

import runpy
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "scripts/prepare-icu-build-tools.py"
PROBE = ROOT / "scripts/verify-icu-catalogs.sh"


class IcuCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tool = runpy.run_path(str(TOOL))

    def test_catalog_probe_rejects_missing_symbol_or_helper(self) -> None:
        loader = self.tool["validate_ubi_loader_report"]
        config = self.tool["validate_pkgdata_config"]
        error = self.tool["IcuToolError"]
        good = (
            "libicutu.so.67 => /lib64/libicutu.so.67 (0x1)\n"
            "libicuuc.so.67 => /lib64/libicuuc.so.67 (0x2)\n"
            "libicui18n.so.67 => /lib64/libicui18n.so.67 (0x3)\n"
            "libicudata.so.67 => /lib64/libicudata.so.67 (0x4)\n"
        )
        self.assertEqual(
            {"/lib64/libicutu.so.67", "/lib64/libicuuc.so.67",
             "/lib64/libicui18n.so.67", "/lib64/libicudata.so.67"},
            loader(good),
        )
        for bad in (
            good.replace("/lib64/libicuuc.so.67", "not found"),
            good.replace("/lib64/libicuuc.so.67", "/tmp/centos/libicuuc.so.67"),
            "libicuuc.so.67 => /lib64/libicuuc.so.67 (0x1)\nundefined symbol: u_init_67\n",
        ):
            with self.subTest(bad=bad), self.assertRaises(error):
                loader(bad)
        valid = "GENCCODE_ASSEMBLY_TYPE=-a gcc\nCOMPILE=gcc -O2 -c\nAR=ar\nRANLIB=ranlib\n"
        config(valid)
        for bad in (
            valid.replace("COMPILE=gcc", "COMPILE=/tmp/unknown-cc"),
            valid.replace("AR=ar", "AR=/tmp/unknown-ar"),
            valid.replace("RANLIB=ranlib", ""),
        ):
            with self.subTest(bad=bad), self.assertRaises(error):
                config(bad)
        source = PROBE.read_text(encoding="utf-8")
        for required in ("icu-config", "pkgconf", "pkgdata.inc", "gcc", "readelf", "make_message_src.sh"):
            self.assertIn(required, source)

    def test_catalog_output_is_repeatable(self) -> None:
        verify = self.tool["verify_catalog_pair"]
        error = self.tool["IcuToolError"]
        with tempfile.TemporaryDirectory() as raw:
            first, second = Path(raw) / "one.a", Path(raw) / "two.a"
            first.write_bytes(b"\x7fELFdeterministic nonempty object")
            second.write_bytes(first.read_bytes())
            verify(first, second)
            second.write_bytes(b"changed")
            with self.assertRaises(error):
                verify(first, second)
            second.write_bytes(b"")
            with self.assertRaises(error):
                verify(first, second)


if __name__ == "__main__":
    unittest.main()
