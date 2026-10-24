"""Tests of the shared icons (bridge LLD R25.3). Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import json
import pathlib
import re
import runpy
import unittest
import xml.etree.ElementTree as ET

import cq_icons as icons

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


class Leftovers(unittest.TestCase):
    """Final review I1: what the expander can't read must stop the build, never ship as an empty button."""

    def fails(self, text: str) -> str:
        with self.assertRaises(SystemExit) as e:
            icons.expand(text, ICONS, "/p")
        return str(e.exception)

    def test_a_marker_it_cannot_read_fails(self) -> None:
        for bad in (
            "<!-- icon:lock -->",
            "<!--Icon:save-->",
            '<!--icon:save title="a>b"-->',
            '<!--icon:save style="x-->y"-->',
        ):
            self.assertIn("/p", self.fails(bad), bad)

    def test_a_table_it_cannot_read_fails(self) -> None:
        self.assertIn("/p", self.fails("/* CQ:ICONS wifi*/"))
        self.assertIn("/p", self.fails("/*cq:icons wifi*/"))

    def test_icon_used_without_a_table_fails(self) -> None:
        self.assertIn("no /*CQ:ICONS", self.fails('x = icon("wifi");'))

    def test_a_literal_name_missing_from_the_table_fails(self) -> None:
        self.assertIn(
            '"menu"',
            self.fails('/*CQ:ICONS wifi*/ a = icon("wifi"); b = icon("menu");'),
        )

    def test_listed_literal_and_computed_names_pass(self) -> None:
        out = icons.expand(
            '/*CQ:ICONS wifi menu*/ a = icon("wifi"); b = icon(NAMES[k]);', ICONS, "/p"
        )
        self.assertIn("var ICONS", out)

    def test_icon_complains_about_an_unknown_name_at_run_time(self) -> None:
        out = icons.expand("/*CQ:ICONS wifi*/", ICONS, "/p")
        self.assertIn("console.error", out)


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


class AddIcon(unittest.TestCase):
    """tools/icons/add_icon.py (final review M3)."""

    def setUp(self) -> None:
        import contextlib
        import io
        import tempfile

        self.tmp = pathlib.Path(tempfile.mkdtemp())
        (self.tmp / "icons").mkdir()
        (self.tmp / "icons" / "dot.svg").write_text(
            '<svg\n  viewBox="0 0 24 24"\n>\n  <circle r="1" />\n</svg>\n'
        )
        self.file = self.tmp / "icons.json"
        self.tool = runpy.run_path(
            str(PROJECT.parents[1] / "tools" / "icons" / "add_icon.py"),
            run_name="add_icon",
        )
        self.out = io.StringIO()
        self.quiet = contextlib.redirect_stdout(self.out)

    def run_tool(self, *argv: str) -> None:
        with self.quiet:
            self.tool["main"](list(argv), self.file)

    def test_from_without_a_folder_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_tool("--from")

    def test_no_names_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            self.run_tool("--from", str(self.tmp))

    def test_the_file_says_the_version_and_a_replaced_icon_is_reported(self) -> None:
        self.file.write_text(
            json.dumps(
                {
                    "source": "lucide-static 0.1.0",
                    "licence": "x",
                    "icons": {"dot": "old"},
                }
            )
        )
        self.run_tool("--from", str(self.tmp), "dot")
        data = json.loads(self.file.read_text())
        self.assertEqual(data["source"], "lucide-static " + self.tool["VERSION"])
        self.assertEqual(data["icons"]["dot"], '<circle r="1"/>')
        self.assertIn("replaced dot", self.out.getvalue())


class TheFile(unittest.TestCase):
    def test_icons_json_is_lucide_and_every_body_is_svg(self) -> None:
        def unique(pairs: list[tuple[str, object]]) -> dict[str, object]:
            keys = [k for k, _ in pairs]
            self.assertEqual(len(keys), len(set(keys)), "a name twice in icons.json")
            return dict(pairs)

        data = json.loads(
            (PROJECT / "web" / "icons.json").read_text(encoding="utf-8"),
            object_pairs_hook=unique,
        )
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
