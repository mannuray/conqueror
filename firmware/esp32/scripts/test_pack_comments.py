"""Comments out when packing (bridge LLD R31.5): room on every page without raising a budget.
Run: cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py'"""

from __future__ import annotations

import pathlib
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest

PROJECT = pathlib.Path(__file__).resolve().parents[1]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"))
PAGES = ("/", "/logo", "/settings", "/help", "/ai")
SCRIPT = re.compile(r"<script>(.*?)</script>", re.DOTALL)


class Comments(unittest.TestCase):
    def test_whole_line_comments_go_and_strings_pre_and_code_stay(self) -> None:
        page = ('<pre>// shown to the reader</pre>\n<style>\n/* a note */\nb { color: red; } /* end */\n</style>\n'
                '<script>\n  // a note\n  var u = "ws://x // y";  // kept: not a whole line\n  var v = 1;\n</script>\n')
        out = _PACK["strip_comments"](page)
        self.assertNotIn("a note", out)
        self.assertNotIn("/* end */", out)
        self.assertIn("<pre>// shown to the reader</pre>", out)
        self.assertIn('var u = "ws://x // y";', out)
        self.assertIn("var v = 1;", out)
        self.assertIn("b { color: red; }", out)

    def test_every_page_is_smaller_and_its_scripts_still_parse(self) -> None:
        node = shutil.which("node")
        self.assertIsNotNone(node, "node is needed for the parse check")
        for uri in PAGES:
            with self.subTest(uri=uri):
                packed = _PACK["assemble_page"](PROJECT, uri).decode()
                whole = _PACK["assemble_page"](PROJECT, uri, keep_comments=True).decode()
                self.assertLess(len(packed), len(whole))
                for i, body in enumerate(SCRIPT.findall(packed)):
                    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
                        f.write(body)
                    r = subprocess.run([node or "node", "--check", f.name], capture_output=True, text=True)
                    pathlib.Path(f.name).unlink()
                    self.assertEqual(r.returncode, 0, f"{uri} script {i}: {r.stderr[:300]}")

    def test_logo_js_never_closes_its_script(self) -> None:  # review: assemble_logo did not escape </
        with tempfile.TemporaryDirectory() as d:
            web = pathlib.Path(d) / "web"
            (web / "generated").mkdir(parents=True)
            (web / "logo.html").write_text("<script><!--CQ:LOGO_JS-->\nvar W = <!--CQ:WORDS-->;</script>", encoding="utf-8")
            (web / "logo.js").write_text("var s = '</b>';\n", encoding="utf-8")
            (web / "generated" / "words.json").write_text("{}", encoding="utf-8")
            out = _PACK["assemble_logo"](pathlib.Path(d)).decode()
            self.assertNotIn("'</b>'", out)
            self.assertIn("'<\\/b>'", out)


if __name__ == "__main__":
    unittest.main()
