"""Tests of the status strip put into /ai, /logo and /settings (bridge LLD R30.4). Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import pathlib
import runpy
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"))


class Strip(unittest.TestCase):
    def test_the_marker_becomes_the_strip_style_markup_and_script(self) -> None:
        out = _PACK["with_strip"]("<div><!--CQ:STRIP--></div>", PROJECT, "/x")
        self.assertNotIn("<!--CQ:STRIP-->", out)
        for ident in ('id="cBatt"', 'id="cAi"', 'id="cFps"', 'id="cHdg"', 'id="cPos"', 'id="cSpd"', 'id="cWifi"', 'id="cUno"'):
            self.assertEqual(out.count(ident), 1, ident)
        self.assertEqual(out.count("window.cqStrip ="), 1)
        script = out[out.index("<script>") : out.rindex("</script>")]
        self.assertNotIn("</", script)  # nothing in strip.js closes its <script>

    def test_a_page_without_the_marker_is_left_as_it_is(self) -> None:
        self.assertEqual(_PACK["with_strip"]("<div></div>", PROJECT, "/x"), "<div></div>")

    def test_ai_has_the_strip_once(self) -> None:
        text = _PACK["assemble_page"](PROJECT, "/ai").decode()
        self.assertEqual(text.count("window.cqStrip ="), 1)
        self.assertEqual(text.count('id="cBatt"'), 1)
        self.assertNotIn("<!--icon:", text)


if __name__ == "__main__":
    unittest.main()
