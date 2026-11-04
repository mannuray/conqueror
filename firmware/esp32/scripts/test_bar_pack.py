"""Tests of the shared top bar put into every page (bridge LLD R29.5). Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import pathlib
import runpy
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"))


class Bar(unittest.TestCase):
    def test_the_marker_becomes_the_bar_style_markup_and_script(self) -> None:
        out = _PACK["with_bar"]("<header><!--CQ:BAR--></header>", PROJECT, "/x")
        self.assertNotIn("<!--CQ:BAR-->", out)
        for ident in ('id="lookBtn"', 'id="menuBtn"', 'id="stop"', 'id="cqMenu"', 'id="cqDim"'):
            self.assertEqual(out.count(ident), 1, ident)
        self.assertEqual(out.count("window.cqBar ="), 1)
        script = out[out.index("<script>") : out.rindex("</script>")]
        self.assertNotIn("</", script)  # nothing in bar.js closes its <script>

    def test_ai_has_the_bar_once_and_no_old_look_button(self) -> None:
        text = _PACK["assemble_page"](PROJECT, "/ai").decode()
        self.assertEqual(text.count('id="lookBtn"'), 1)
        self.assertEqual(text.count('id="stop"'), 1)
        self.assertNotIn('id="themeBtn"', text)
        self.assertNotIn("<!--icon:", text)  # the bar's icons are put in too


if __name__ == "__main__":
    unittest.main()
