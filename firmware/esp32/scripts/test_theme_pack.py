"""Tests of the shared look put into every page (bridge LLD R28.5). Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import pathlib
import runpy
import tempfile
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"))


class Theme(unittest.TestCase):
    def test_the_marker_becomes_the_theme_style_and_script(self) -> None:
        out = _PACK["with_theme"]("<head><!--CQ:THEME--></head>", PROJECT, "/x")
        self.assertNotIn("<!--CQ:THEME-->", out)
        style = out[out.index("<style>") : out.index("</style>")]
        self.assertIn('data-theme="play"', style)
        script = out[out.index("<script>") : out.rindex("</script>")]
        self.assertIn("cqTheme", script)
        self.assertNotIn("</", script)  # nothing in theme.js closes its <script>

    def test_a_page_without_the_marker_fails_the_build_naming_it(self) -> None:
        with self.assertRaises(SystemExit) as e:
            _PACK["with_theme"]("<head></head>", PROJECT, "/x")
        self.assertIn("/x", str(e.exception))

    def test_every_page_has_the_theme_once(self) -> None:
        for uri in ("/", "/settings", "/logo", "/ai", "/help"):
            with self.subTest(uri=uri):
                text = _PACK["assemble_page"](PROJECT, uri).decode()
                self.assertEqual(text.count("window.cqTheme ="), 1)

    def test_a_style_end_in_theme_css_fails_the_build(self) -> None:  # final review M8
        with tempfile.TemporaryDirectory() as d:
            web = pathlib.Path(d) / "web"
            web.mkdir()
            (web / "theme.css").write_text("/* </style> */\n", encoding="utf-8")
            (web / "theme.js").write_text("var cqTheme = {};\n", encoding="utf-8")
            with self.assertRaises(SystemExit) as e:
                _PACK["with_theme"]("<head><!--CQ:THEME--></head>", pathlib.Path(d), "/x")
            self.assertIn("</style", str(e.exception))

    def test_a_page_gets_only_its_own_sunny_block(self) -> None:  # final review M9: /logo's budget
        logo = _PACK["assemble_page"](PROJECT, "/logo").decode()
        drive = _PACK["assemble_page"](PROJECT, "/").decode()
        self.assertIn("--move:", logo)           # the code colours: /logo only
        self.assertNotIn("--move:", drive)
        self.assertIn("--knob-a:", drive)        # the sticks: the drive page only
        self.assertNotIn("--knob-a:", logo)
        self.assertIn("--danger:", logo)         # the shared names: every page
        self.assertIn("--danger:", drive)


if __name__ == "__main__":
    unittest.main()
