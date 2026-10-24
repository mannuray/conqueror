"""Tests of the shared icons (bridge LLD R25.3). Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import json
import pathlib
import re
import runpy
import unittest
import xml.etree.ElementTree as ET

import icons

PROJECT = pathlib.Path(__file__).resolve().parents[1]
ICONS = {
    "save": '<path d="M1 2"/>',
    "wifi": '<path d="M3 4"/><circle cx="1" cy="1" r="1"/>',
    "menu": '<path d="M5 6"/>',
}


class Markers(unittest.TestCase):
    def test_a_marker_becomes_the_svg_with_its_body(self) -> None:
        out = icons.expand_markers("<b><!--icon:save--></b>", ICONS, "/x")
        self.assertEqual(out, '<b><svg viewBox="0 0 24 24"><path d="M1 2"/></svg></b>')

    def test_attributes_are_kept_in_order(self) -> None:
        out = icons.expand_markers(
            '<!--icon:menu id="m" class="hidden"-->', ICONS, "/x"
        )
        self.assertEqual(
            out, '<svg viewBox="0 0 24 24" id="m" class="hidden"><path d="M5 6"/></svg>'
        )

    def test_an_unknown_name_fails_naming_the_page(self) -> None:
        with self.assertRaises(SystemExit) as e:
            icons.expand_markers("<!--icon:wifi-hi-->", ICONS, "/logo")
        self.assertIn('/logo uses icon "wifi-hi"', str(e.exception))

    def test_a_marker_without_a_name_fails(self) -> None:
        with self.assertRaises(SystemExit):
            icons.expand_markers("<!--icon:-->", ICONS, "/x")

    def test_other_comments_are_left_alone(self) -> None:
        text = "<!--CQ:LOGO_JS--><!-- a note -->"
        self.assertEqual(icons.expand_markers(text, ICONS, "/x"), text)


class Table(unittest.TestCase):
    def test_the_table_holds_only_the_listed_icons_and_icon(self) -> None:
        out = icons.expand_table(
            "<script>/*CQ:ICONS wifi menu*/ x();</script>", ICONS, "/x"
        )
        m = re.search(r"var ICONS = (\{.*?\});", out)
        assert m is not None
        self.assertEqual(
            json.loads(m.group(1).replace("<\\/", "</")),
            {"wifi": ICONS["wifi"], "menu": ICONS["menu"]},
        )
        self.assertIn("function icon(n) {", out)
        self.assertNotIn("CQ:ICONS", out)

    def test_an_unknown_name_in_the_table_fails(self) -> None:
        with self.assertRaises(SystemExit) as e:
            icons.expand_table("/*CQ:ICONS wifi nope*/", ICONS, "/settings")
        self.assertIn('/settings uses icon "nope"', str(e.exception))

    def test_a_second_table_fails(self) -> None:
        with self.assertRaises(SystemExit):
            icons.expand_table("/*CQ:ICONS wifi*/ /*CQ:ICONS menu*/", ICONS, "/x")

    def test_the_table_never_closes_its_script(self) -> None:
        out = icons.expand_table("/*CQ:ICONS x*/", {"x": "<text>a</text>"}, "/x")
        self.assertNotIn("</text>", out)


class Lucide(unittest.TestCase):
    def test_a_lucide_file_gives_its_compact_body(self) -> None:
        svg = '<!-- @license -->\n<svg\n  class="lucide"\n  viewBox="0 0 24 24"\n>\n  <path d="M12 20h.01" />\n  <rect x="1" />\n</svg>\n'
        self.assertEqual(icons.lucide_body(svg), '<path d="M12 20h.01"/><rect x="1"/>')


class TheFile(unittest.TestCase):
    def test_icons_json_is_lucide_and_every_body_is_svg(self) -> None:
        data = json.loads((PROJECT / "web" / "icons.json").read_text(encoding="utf-8"))
        self.assertEqual(data["source"], "lucide-static 0.460.0")
        self.assertEqual(list(data["icons"]), sorted(data["icons"]))
        for name, body in data["icons"].items():
            ET.fromstring(
                f'<svg xmlns="http://www.w3.org/2000/svg">{body}</svg>'
            )  # raises on bad markup
            self.assertRegex(name, r"^[a-z0-9-]+$")

    def test_every_page_packs_with_no_marker_left(self) -> None:
        pack = runpy.run_path(
            str(PROJECT / "scripts" / "pack_web.py"), run_name="pack_web"
        )
        for uri in ("/", "/settings", "/logo", "/help"):
            text = pack["assemble_page"](PROJECT, uri).decode()
            self.assertNotIn("<!--icon:", text, uri)
            self.assertNotIn("CQ:ICONS", text, uri)


if __name__ == "__main__":
    unittest.main()
