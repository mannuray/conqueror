"""Every page's <head> names the tank's icon and a browser-bar colour (owner 2026-11-04: the /ai page had no icon).
Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import pathlib
import runpy
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"))
PAGES = ("/", "/logo", "/settings", "/help", "/ai")


class Head(unittest.TestCase):
    def test_every_page_has_the_icon_and_a_browser_bar_colour(self) -> None:
        for uri in PAGES:
            with self.subTest(uri=uri):
                text = _PACK["assemble_page"](PROJECT, uri).decode()
                head = text[: text.index("</head>")]
                self.assertIn('<link rel="icon" href="/icon-192.png">', head)
                self.assertIn('<link rel="apple-touch-icon" href="/icon-192.png">', head)
                self.assertIn('<meta name="theme-color"', head)


if __name__ == "__main__":
    unittest.main()
