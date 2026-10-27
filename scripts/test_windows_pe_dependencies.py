"""Bounded PE fixtures test import analysis, never runtime or media behavior."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import windows_pe_dependencies as deps


def pe_fixture(imports=(), delayed=()):
    data = bytearray(4096)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 1, 0, 0, 0, 240, 0)
    optional = 0x98
    struct.pack_into("<H", data, optional, 0x20B)
    struct.pack_into("<I", data, optional + 108, 16)
    struct.pack_into("<IIII", data, optional + 240 + 8, 3584, 0x1000, 3584, 512)
    cursor = 1536
    for index, names, stride, position in ((1, imports, 20, 512), (13, delayed, 32, 1024)):
        if not names:
            continue
        rva = 0x1000 + position - 512
        struct.pack_into("<II", data, optional + 112 + index * 8, rva, stride * (len(names) + 1))
        for number, name in enumerate(names):
            encoded = name.encode("ascii") + b"\0"
            data[cursor:cursor + len(encoded)] = encoded
            values = [0] * (stride // 4)
            if index == 13:
                values[0] = 1
            values[1 if index == 13 else 3] = 0x1000 + cursor - 512
            struct.pack_into("<" + "I" * len(values), data, position + stride * number, *values)
            cursor += len(encoded)
    return bytes(data)


class ImportClosureTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bundle = self.root / "bundle"
        self.bundle.mkdir()
        for name in ("gcp-desktop.exe", "gcp-livekit-voice.exe", "gcp-livekit-screen.exe", "livekit.dll", "livekit_ffi.dll"):
            (self.bundle / name).write_bytes(pe_fixture(["KERNEL32.dll"]))
        (self.bundle / "gcp-desktop.exe").write_bytes(pe_fixture(["livekit.dll"], ["api-ms-win-core-synch-l1-2-0.dll"]))
        self.inventory = {"commit_sha": "a" * 40, "artifact_kind": "windows_distribution_build_inventory",
                          "attribution_complete": False, "production_qualified": False,
                          "physical_media_verified": False, "clean_consumer_windows_verified": False}
        self.save_inventory()

    def save_inventory(self):
        self.inventory["executables"] = [{"path": p.name, "sha256": deps.digest(p), "bytes": p.stat().st_size}
                                         for p in self.bundle.iterdir() if p.suffix in {".exe", ".dll"}]
        (self.bundle / "DISTRIBUTION-INVENTORY.json").write_text(json.dumps(self.inventory))

    def test_regular_and_delayed_imports_are_byte_bound(self):
        result = deps.analyze(self.bundle, "a" * 40)
        row = next(x for x in result["executables"] if x["path"] == "gcp-desktop.exe")
        self.assertEqual(row["imports"], [{"name": "livekit.dll", "kind": "bundled", "path": "livekit.dll"}])
        self.assertEqual(row["delay_imports"][0]["kind"], "windows_system")
        self.assertTrue(result["declared_imports_resolved"])
        self.assertFalse(result["all_dynamic_loading_verified"])
        self.assertFalse(result["production_qualified"])

    def test_missing_dll_is_detected_even_if_the_source_inventory_is_regenerated(self):
        (self.bundle / "livekit.dll").unlink()
        (self.bundle / "another.dll").write_bytes(pe_fixture())
        self.save_inventory()
        with self.assertRaisesRegex(ValueError, "Unresolved dependency"):
            deps.analyze(self.bundle, "a" * 40)

    def test_unknown_runtime_is_never_inferred_to_be_an_os_dll(self):
        (self.bundle / "livekit.dll").write_bytes(pe_fixture(["private-development-runtime.dll"]))
        self.save_inventory()
        with self.assertRaisesRegex(ValueError, "Unresolved dependency"):
            deps.analyze(self.bundle, "a" * 40)

    def test_source_digest_identity_and_false_claims_rejected(self):
        for key in ("attribution_complete", "production_qualified", "physical_media_verified", "clean_consumer_windows_verified"):
            for bad in (True, "False", 0, None):
                self.inventory[key] = bad; self.save_inventory()
                with self.assertRaisesRegex(ValueError, "qualification claim"):
                    deps.analyze(self.bundle, "a" * 40)
            self.inventory[key] = False
        self.save_inventory()
        with self.assertRaisesRegex(ValueError, "identity"):
            deps.analyze(self.bundle, "b" * 40)
        with (self.bundle / "livekit.dll").open("ab") as stream:
            stream.write(b"changed")
        with self.assertRaisesRegex(ValueError, "digest"):
            deps.analyze(self.bundle, "a" * 40)

    def test_malformed_pe_architecture_and_rva_rejected(self):
        original = pe_fixture(["KERNEL32.dll"])
        cases = [b"not PE", original[:150], original[:1024]]
        bad = bytearray(original); struct.pack_into("<H", bad, 0x84, 0x14C); cases.append(bad)
        bad = bytearray(original); struct.pack_into("<I", bad, 512 + 12, 0xFFFF0000); cases.append(bad)
        for data in cases:
            (self.bundle / "livekit.dll").write_bytes(data)
            self.save_inventory()
            with self.assertRaises(ValueError):
                deps.analyze(self.bundle, "a" * 40)

    def test_path_aliases_and_links_rejected(self):
        for name in ("../x.dll", "/x.dll", "C:/x.dll", "dir//x.dll", "dir/x.dll.", "dir/CON.dll", "x.dll:stream", "a\\x.dll"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                deps.safe_relative(name)
        if __import__("os").name != "nt":
            (self.bundle / "linked.dll").symlink_to(self.bundle / "livekit.dll")
            with self.assertRaisesRegex(ValueError, "links"):
                deps.analyze(self.bundle, "a" * 40)


if __name__ == "__main__":
    unittest.main()


