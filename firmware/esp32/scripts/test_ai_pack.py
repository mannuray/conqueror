"""Tests of the /ai page's ROOM tab packing (bridge LLD R32). Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import gzip
import pathlib
import runpy
import shutil
import tempfile
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"))


class AiPage(unittest.TestCase):
    def test_room_js_is_packed_in_once(self) -> None:
        out = _PACK["assemble_page"](PROJECT, "/ai").decode()
        self.assertNotIn("<!--CQ:ROOM_JS-->", out)
        self.assertEqual(out.count("window.cqRoom ="), 1)
        self.assertLess(out.index("var AiCore ="), out.index("var cqRoomLib ="))  # room.js uses AiCore

    def test_no_inside_tab(self) -> None:
        out = _PACK["assemble_page"](PROJECT, "/ai").decode()
        self.assertNotIn('data-tab="inside"', out)
        self.assertNotIn('id="tInside"', out)

    def test_ai_within_its_30_kb(self) -> None:
        self.assertEqual(_PACK["AI_BUDGET"], 30 * 1024)
        size = len(gzip.compress(_PACK["assemble_page"](PROJECT, "/ai"), compresslevel=9, mtime=0))
        self.assertLessEqual(size, 30 * 1024)

    def test_a_missing_marker_is_named(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = pathlib.Path(d)
            shutil.copytree(PROJECT / "web", root / "web")
            page = root / "web" / "ai.html"
            page.write_text(page.read_text(encoding="utf-8").replace("<!--CQ:ROOM_JS-->", ""), encoding="utf-8")
            with self.assertRaises(SystemExit) as e:
                _PACK["assemble_ai"](root)
            self.assertIn("<!--CQ:ROOM_JS-->", str(e.exception))


if __name__ == "__main__":
    unittest.main()
