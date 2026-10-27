"""Check retained upstream bytes and source-catalog references without network access."""
import hashlib
import json
from pathlib import Path
import unittest

from windows_pe_dependencies import ROOT, safe_relative


class NoticeTests(unittest.TestCase):
    def test_all_pinned_material_bytes_and_catalog_references(self):
        root = ROOT / "third_party/notices"
        catalog = json.loads((root / "catalog.json").read_text())
        self.assertEqual(catalog["coverage"], "expanded_source_notices_build_review_pending")
        seen = set()
        for row in catalog["files"]:
            name = safe_relative(row["path"])
            self.assertNotIn(name.casefold(), seen)
            seen.add(name.casefold())
            data = (root / name).read_bytes()
            self.assertEqual(len(data), row["bytes"], name)
            self.assertEqual(hashlib.sha256(data).hexdigest(), row["sha256"], name)
        sources = json.loads((root / "source-catalog.json").read_text())
        self.assertEqual(sources["coverage"], "pinned_source_notice_superset_not_build_complete")
        for row in sources["qt_components"]:
            for name in [row["attribution_path"], *row["materials"]]:
                self.assertIn(name.casefold(), seen)
        self.assertIn("livekit-rust/soxr-sys/build.rs", seen)
        self.assertIn("LGPL v2.1", (root / "livekit-rust/soxr-sys/src/soxr.c").read_text())
        actual = {p.relative_to(root).as_posix().casefold() for p in root.rglob("*") if p.is_file()}
        self.assertEqual(actual, seen | {"catalog.json", "readme.md"})


if __name__ == "__main__":
    unittest.main()
