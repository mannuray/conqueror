# Plan N7 — One shared icon file for the tank's pages — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every icon on the tank's pages comes from one file of standard Lucide icons (`web/icons.json`), put into each page when the firmware is packed. Drive and Settings look the same, apart from three small icons. Settings and Logo get Lucide WiFi-strength and battery icons, and Logo's text icons (`▂▄▆█`, `● UNO`, `▶ RUN`) become Lucide.

**Architecture:**
- **`scripts/icons.py`** (pure) expands two kinds of markers in a page:
  - `<!--icon:NAME attrs-->` becomes an inline `<svg>`;
  - `/*CQ:ICONS a b c*/` becomes a small `ICONS` table plus `icon(name)`, with only that page's icons.
- **`pack_web.py`** runs every page through it (`assemble_page`). An unknown name fails the build.
- **`tools/icons/add_icon.py`** adds icons from lucide-static 0.460.0. Only a developer runs it.

**Tech Stack:** Python 3 stdlib (`unittest`, `json`, `re`) for the packer; plain JS / HTML pages; Node test runner; Playwright `run_code` checks on `web/test/fake_tank.py`.

**Spec:** `docs/lld/esp32-bridge-lld.md` R25 (approved 2026-10-24); Tank Logo LLD §23.9; design-notes §148.

**Built from a tested draft:** branch `n7-draft`.
- Packer tests 12/12; Node 54/54.
- Browser checks pass: Logo laptop + phone, both Settings camera checks, the new icons check.
- Screenshots: Drive (3 views) and Settings (4 tabs) are pixel-identical before and after, except three icons (Ruling 2).
- Sizes gzipped: `/` 20 016 → 20 175 B, `/settings` 22 164 → 22 523 B, `/logo` 22 990 → 24 514 B.
- Bridge flash 44.6 %.

## Global Constraints
- **Icons:** lucide-static **0.460.0** (ISC), bodies copied unchanged; names are Lucide's.
- **The `<svg>` tag stays** `<svg viewBox="0 0 24 24"` + the marker's attributes + `>`, so the pages' CSS still applies.
- **An unknown icon name, an empty marker or a second `CQ:ICONS` fails the build**, naming the page.
- **Budgets (gzipped):** `/` 25 KB, `/settings` 36 KB, **`/logo` 26 KB** (was 24), `/help` 16 KB.
- **The build never touches the network.** Only `add_icon.py` does, and only when a developer runs it.
- **Rules copied from Drive:**
  - WiFi icon by RSSI: > −55 `wifi`, > −65 `wifi-high`, > −75 `wifi-low`, else `wifi-zero`; no link `wifi-off`.
  - Battery (2S) by charge: ≥ 7.85 V `battery-full`, ≥ 7.4 `battery-medium`, ≥ 7.0 `battery-low`, else `battery-warning`; under 5.5 V it's USB, `plug`.
- **Hardware (Task 6):** the bridge flashed over WiFi; never print the OTA password; nothing drives.
- **Browser checks:** run each in a fresh browser (close the Playwright page first). A check's injected scripts stay on its tab.

## Review Focus
1. **A page must never ship with a missing icon** (an empty button). Pinned by `test_every_page_packs_with_no_marker_left` and the unknown-name tests.
2. **Drive and Settings must look the same** except the three listed icons. Pinned by the before / after screenshot comparison (Task 4 Step 5).
3. **`icon()` must be defined before a script uses it:** Settings builds its `SVG_*` constants at start-up. Pinned by the Settings camera checks and the icons check running without errors.
4. **PlatformIO runs `pack_web.py` through SCons, which may not set `__file__`.** Pinned by building the bridge (Task 3 Step 6).
5. **The browser checks must use the packed pages.** The camera checks used to load the raw `settings.html`, which now has markers. Pinned by moving them to `fake_tank.py`.

---

### Task 1: The icon expander (pure)

**Files:**
- Create: `firmware/esp32/scripts/icons.py`, `firmware/esp32/scripts/test_icons.py`

**Interfaces:**
- Produces:
  - `icons.load(path) -> dict[str, str]`;
  - `icons.expand_markers(text, icons, page) -> str`;
  - `icons.expand_table(text, icons, page) -> str`;
  - `icons.expand(text, icons, page) -> str`;
  - `icons.lucide_body(svg_text) -> str`.

- [ ] **Step 1: Write the tests**, `firmware/esp32/scripts/test_icons.py`, in full. The `TheFile` tests also cover Tasks 2 and 3; they fail until then.

```python
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
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py' 2>&1 | tail -3`
Expected: `ModuleNotFoundError: No module named 'icons'` (1 error).

- [ ] **Step 3: Write `firmware/esp32/scripts/icons.py`**

```python
"""The tank pages' shared icons (bridge LLD R25): Lucide bodies from web/icons.json put into each page when packing.

- `<!--icon:NAME attrs-->` becomes `<svg viewBox="0 0 24 24" attrs>BODY</svg>` (the tag the pages always used).
- `/*CQ:ICONS a b c*/` in a script becomes `var ICONS = {...}` with only those icons, and `icon(name)`.
An unknown name fails the build, naming the page. Pure: no files read here except by `load`.
"""

from __future__ import annotations

import json
import pathlib
import re

MARKER = re.compile(r"<!--icon:([^\s>]*?)((?:\s[^>]*?)?)-->")
TABLE = re.compile(r"/\*CQ:ICONS([^*]*)\*/")
SVG_OPEN = '<svg viewBox="0 0 24 24"'


def load(path: pathlib.Path) -> dict[str, str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    return dict(data["icons"])


def _body(icons: dict[str, str], name: str, page: str) -> str:
    if not name:
        raise SystemExit(f"pack_web: {page} has an icon marker without a name")
    if name not in icons:
        raise SystemExit(
            f'pack_web: {page} uses icon "{name}", which is not in web/icons.json (tools/icons/add_icon.py)'
        )
    return icons[name]


def expand_markers(text: str, icons: dict[str, str], page: str) -> str:
    def one(m: re.Match[str]) -> str:
        body = _body(icons, m.group(1), page)
        return SVG_OPEN + m.group(2).rstrip() + ">" + body + "</svg>"

    return MARKER.sub(one, text)


def expand_table(text: str, icons: dict[str, str], page: str) -> str:
    found = TABLE.findall(text)
    if len(found) > 1:
        raise SystemExit(f"pack_web: {page} has more than one /*CQ:ICONS*/ table")
    if not found:
        return text
    table = {name: _body(icons, name, page) for name in found[0].split()}
    code = (
        "var ICONS = "
        + json.dumps(table, separators=(",", ":")).replace("</", "<\\/")
        + "; "
        "function icon(n) { return '" + SVG_OPEN + '>\' + ICONS[n] + "</svg>"; }'
    )
    return TABLE.sub(lambda _m: code, text)


def expand(text: str, icons: dict[str, str], page: str) -> str:
    return expand_markers(expand_table(text, icons, page), icons, page)


def lucide_body(svg: str) -> str:
    """The inside of a lucide-static icon file, compact (as in the pages): `<path d="..."/><rect .../>`."""
    inner = svg[svg.index(">", svg.index("<svg")) + 1 : svg.rindex("</svg>")]
    inner = re.sub(r"\s*/>", "/>", inner.strip())
    return re.sub(r">\s+<", "><", inner)
```

- [ ] **Step 4: Run the tests**

Run: `cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py' 2>&1 | tail -3`
Expected: 12 tests; the 10 in `Markers`, `Table` and `Lucide` pass. `TheFile` fails: `icons.json` doesn't exist yet, and there's no `assemble_page`.

- [ ] **Step 5: Lint and commit**

```bash
uvx ruff check firmware/esp32/scripts/icons.py firmware/esp32/scripts/test_icons.py && uvx ruff format --check firmware/esp32/scripts/icons.py firmware/esp32/scripts/test_icons.py
git add firmware/esp32/scripts/icons.py firmware/esp32/scripts/test_icons.py
git commit -m "feat(web): icons.py - shared Lucide icon markers for the pages (bridge LLD R25.1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: `icons.json` and `add_icon.py`

**Files:**
- Create: `tools/icons/add_icon.py`, `firmware/esp32/web/icons.json` (written by the tool)

**Interfaces:**
- Consumes: `icons.lucide_body` (Task 1).
- Produces: `web/icons.json` = `{"source": "lucide-static 0.460.0", "licence": …, "icons": {name: body}}`, sorted by name.

- [ ] **Step 1: Write `tools/icons/add_icon.py`**

```python
"""Add Lucide icons to firmware/esp32/web/icons.json (bridge LLD R25.1 item 2).

    python3 tools/icons/add_icon.py radar plug           # fetched from unpkg, lucide-static 0.460.0
    python3 tools/icons/add_icon.py --from DIR radar     # from an unpacked lucide-static package (DIR/icons/*.svg)

Only a developer runs this; the firmware build never touches the network. The version is pinned here.
"""

from __future__ import annotations

import json
import pathlib
import re
import sys
import urllib.request

VERSION = "0.460.0"
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware" / "esp32" / "scripts"))
import icons

FILE = ROOT / "firmware" / "esp32" / "web" / "icons.json"


def fetch(name: str, local: pathlib.Path | None) -> str:
    if not re.fullmatch(r"[a-z0-9-]+", name):
        raise SystemExit(f"add_icon: bad name {name!r}")
    if local is not None:
        return (local / "icons" / f"{name}.svg").read_text(encoding="utf-8")
    url = f"https://unpkg.com/lucide-static@{VERSION}/icons/{name}.svg"
    with urllib.request.urlopen(
        url, timeout=20
    ) as r:  # a fixed https host, pinned version
        return r.read().decode("utf-8")


def main(argv: list[str]) -> None:
    local = None
    if argv[:1] == ["--from"]:
        local, argv = pathlib.Path(argv[1]), argv[2:]
    data = {
        "source": f"lucide-static {VERSION}",
        "licence": "ISC, (c) Lucide Contributors (portions (c) Cole Bemis, Feather, MIT)",
        "icons": {},
    }
    if FILE.exists():
        data = json.loads(FILE.read_text(encoding="utf-8"))
    for name in argv:
        data["icons"][name] = icons.lucide_body(fetch(name, local))
    data["icons"] = dict(sorted(data["icons"].items()))
    FILE.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    print(f"add_icon: {len(argv)} added, {len(data['icons'])} in {FILE.name}")


if __name__ == "__main__":
    main(sys.argv[1:])
```

- [ ] **Step 2: Add the 63 icons the pages use.** From unpkg:

```bash
python3 tools/icons/add_icon.py activity arrow-left-right arrow-up battery battery-full battery-low battery-medium \
  battery-warning bookmark bot camera check chevron-left chevrons-up-down circle-dot circle-help circle-plus code cpu \
  download eye-off gamepad-2 gauge hand info joystick locate-fixed lock menu music navigation-2 octagon-x pencil play \
  plug plus radar radio-tower refresh-cw rotate-ccw router ruler save scan-eye scroll-text send settings shield \
  smartphone smile sparkles step-forward trash triangle-alert video volume-2 volume-x wifi wifi-high wifi-low wifi-off \
  wifi-zero wrench
```
(Or offline, from an unpacked `npm pack lucide-static@0.460.0` in its own folder: `--from DIR` first.)
Expected: `add_icon: 63 added, 63 in icons.json`; the file is about 11.7 KB.

- [ ] **Step 3: Run the tests**

Run: `cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py' 2>&1 | tail -3`
Expected: `test_icons_json_is_lucide_and_every_body_is_svg` now passes; only `test_every_page_packs_with_no_marker_left` fails (no `assemble_page`).

- [ ] **Step 4: Lint and commit**

```bash
uvx ruff check tools/icons/add_icon.py && uvx ruff format --check tools/icons/add_icon.py
git add tools/icons/add_icon.py firmware/esp32/web/icons.json
git commit -m "feat(web): icons.json - the 63 Lucide icons the pages use, and tools/icons/add_icon.py (bridge LLD R25.1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: Every page through the icons when packing

**Files:**
- Modify: `firmware/esp32/scripts/pack_web.py`, `firmware/esp32/web/test/fake_tank.py`, `firmware/esp32/web/test/camera_centre_check.js`, `firmware/esp32/web/test/camera_tuning_check.js`

**Interfaces:**
- Consumes: `icons.load`, `icons.expand` (Task 1); `web/icons.json` (Task 2).
- Produces: `assemble_page(project, uri) -> bytes`, with `uri` one of `/`, `/settings`, `/logo`, `/help`. The fake tank serves `/`, `/settings` and `/logo` packed.

- [ ] **Step 1: `pack_web.py`.** Apply this diff:

```diff
diff --git a/firmware/esp32/scripts/pack_web.py b/firmware/esp32/scripts/pack_web.py
index 5ac71eb..9778999 100644
--- a/firmware/esp32/scripts/pack_web.py
+++ b/firmware/esp32/scripts/pack_web.py
@@ -7,7 +7,9 @@
   src/generated/help_protocol.html and <!--CQ:LOGO--> by web/help_logo.html (Task 9);
   skipped while web/help.html does not exist.
 - /logo (Plan N4, Tank Logo LLD §21): web/logo.html with <!--CQ:LOGO_JS--> replaced by
-  web/logo.js and <!--CQ:WORDS--> by web/generated/words.json. Budget 24 KB gzipped.
+  web/logo.js and <!--CQ:WORDS--> by web/generated/words.json. Budget 26 KB gzipped (bridge LLD R25).
+- Every page: its icons from web/icons.json (scripts/icons.py, bridge LLD R25) - <!--icon:NAME--> markers and one
+  /*CQ:ICONS ...*/ table; an unknown icon fails the build.
 Also runnable alone: python3 scripts/pack_web.py [project_dir]
 """
 
@@ -19,9 +21,14 @@ import struct
 import sys
 import zlib
 
+# PlatformIO runs this file through SCons, which may not set __file__; its working directory is the project.
+_HERE = pathlib.Path(__file__).resolve().parent if "__file__" in globals() else pathlib.Path.cwd() / "scripts"
+sys.path.insert(0, str(_HERE))
+import icons
+
 PAGE_BUDGET = 25 * 1024
 HELP_BUDGET = 16 * 1024
-LOGO_BUDGET = 24 * 1024  # Plan N4 (Tank Logo LLD §21.9): /logo, gzipped, words included
+LOGO_BUDGET = 26 * 1024  # Plan N4 (Tank Logo LLD §21.9): /logo, gzipped, words included; R25: 26 KB with icons
 SETTINGS_BUDGET = 36 * 1024  # Plan M: /settings, gzipped; Plan O (LLD R13.1): 36 KB for four tabs
 
 
@@ -89,6 +96,21 @@ def assemble_logo(project: pathlib.Path) -> bytes:
     return text.replace("<!--CQ:LOGO_JS-->", code).replace("<!--CQ:WORDS-->", words).encode()
 
 
+def assemble_page(project: pathlib.Path, uri: str) -> bytes:
+    """A page as served (before gzip), its icons put in (bridge LLD R25). /help may be missing: b""."""
+    web = project / "web"
+    if uri == "/":
+        raw = (web / "basic_drive.html").read_bytes()
+    elif uri == "/settings":
+        raw = (web / "settings.html").read_bytes()
+    elif uri == "/logo":
+        raw = assemble_logo(project)
+    else:
+        raw = assemble_help(project) or b""
+    table = icons.load(web / "icons.json")
+    return icons.expand(raw.decode("utf-8"), table, uri).encode("utf-8")
+
+
 def c_array(name: str, data: bytes) -> str:
     lines = [f"inline const uint8_t {name}[] = {{"]
     for i in range(0, len(data), 20):
@@ -101,7 +123,7 @@ def build(project: pathlib.Path) -> None:
     web = project / "web"
     # (url, content type, data, gzipped)
     assets: list[tuple[str, str, bytes, bool]] = [
-        ("/", "text/html; charset=utf-8", gz((web / "basic_drive.html").read_bytes()), True),
+        ("/", "text/html; charset=utf-8", gz(assemble_page(project, "/")), True),
         (
             "/manifest.webmanifest",
             "application/manifest+json",
@@ -112,15 +134,15 @@ def build(project: pathlib.Path) -> None:
     ]
     if len(assets[0][2]) > PAGE_BUDGET:
         raise SystemExit(f"pack_web: page {len(assets[0][2])} B gzipped > {PAGE_BUDGET} B budget")
-    settings = gz((web / "settings.html").read_bytes())  # Plan M (spec 02 Part I)
+    settings = gz(assemble_page(project, "/settings"))  # Plan M (spec 02 Part I)
     if len(settings) > SETTINGS_BUDGET:
         raise SystemExit(f"pack_web: /settings {len(settings)} B gzipped > {SETTINGS_BUDGET} B budget")
     assets.append(("/settings", "text/html; charset=utf-8", settings, True))
-    logo = gz(assemble_logo(project))
+    logo = gz(assemble_page(project, "/logo"))
     if len(logo) > LOGO_BUDGET:
         raise SystemExit(f"pack_web: /logo {len(logo)} B gzipped > {LOGO_BUDGET} B budget")
     assets.append(("/logo", "text/html; charset=utf-8", logo, True))
-    help_page = assemble_help(project)
+    help_page = assemble_page(project, "/help") if (web / "help.html").exists() else None
     if help_page is not None:
         packed = gz(help_page)
         if len(packed) > HELP_BUDGET:
```

- [ ] **Step 2: Run the tests**

Run: `cd firmware/esp32 && python3 -m unittest discover -s scripts -p 'test_*.py' 2>&1 | tail -3 && python3 scripts/pack_web.py | tail -1`
Expected:
- `Ran 12 tests … OK`;
- the sizes are as before, since no page has a marker yet: `/ 20016 B … /settings 22164 B, /logo 22990 B`.

- [ ] **Step 3: The fake tank serves the packed pages.** Apply this diff:

```diff
diff --git a/firmware/esp32/web/test/fake_tank.py b/firmware/esp32/web/test/fake_tank.py
index 5e9ad7b..c8957ce 100644
--- a/firmware/esp32/web/test/fake_tank.py
+++ b/firmware/esp32/web/test/fake_tank.py
@@ -25,7 +25,7 @@ from urllib.parse import urlsplit
 
 PROJECT = pathlib.Path(__file__).resolve().parents[2]
 _PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"), run_name="pack_web")
-assemble_logo = cast(Callable[[pathlib.Path], bytes], _PACK["assemble_logo"])
+assemble_page = cast(Callable[[pathlib.Path, str], bytes], _PACK["assemble_page"])  # icons in (bridge LLD R25)
 gz = cast(Callable[[bytes], bytes], _PACK["gz"])
 
 PROGRAMS: dict[str, str] = {"square": "REPEAT 4 [ FD 50 RT 90 ]\n"}
@@ -123,8 +123,8 @@ class Handler(BaseHTTPRequestHandler):
 
     def do_GET(self) -> None:
         path = self.where()
-        if path == "/logo":
-            return self.reply(200, gz(assemble_logo(PROJECT)), "text/html; charset=utf-8", True)
+        if path in ("/", "/settings", "/logo"):  # the packed pages, as the bridge serves them
+            return self.reply(200, gz(assemble_page(PROJECT, path)), "text/html; charset=utf-8", True)
         if path == "/fake/stats":  # for the browser check: how many requests were refused busy
             return self.json({"busy": state["busy"]})
         if path == "/logo/programs":
```

- [ ] **Step 4: The camera checks use the fake tank** (they loaded the raw `settings.html`, which will have markers). In both `camera_centre_check.js` and `camera_tuning_check.js`:
  - the comment line 2 becomes `// Run the stand-in tank (python3 firmware/esp32/web/test/fake_tank.py 8765) and open /settings (packed, icons in).`;
  - `"http://127.0.0.1:8766/settings.html?v="` becomes `"http://127.0.0.1:8765/settings?v="`.

- [ ] **Step 5: Run both camera checks** (fake tank on :8765, a fresh browser each).
Expected: the values in their comments:
- camera centre: `reads [27,28]`, `shown "pan -5.0° · tilt 0.0°"`, … `rehome 1`, `wide true`;
- camera tuning: `wbShown ["auto"]` … `wide true`.

- [ ] **Step 6: Build the bridge** (SCons runs `pack_web.py`). Copy `include/secrets.h` in, and delete it afterwards.

Run: `cd firmware/esp32 && ~/.platformio/penv/bin/pio run -e bridge 2>&1 | grep -E "pack_web|SUCCESS|FAILED"`
Expected: the `pack_web:` line, then `SUCCESS`.

- [ ] **Step 7: Commit**

```bash
git add firmware/esp32/scripts/pack_web.py firmware/esp32/web/test/fake_tank.py firmware/esp32/web/test/camera_centre_check.js firmware/esp32/web/test/camera_tuning_check.js
git commit -m "feat(web): every page packed through the shared icons; /logo budget 26 KB (bridge LLD R25.1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Drive and Settings from the shared icons

**Files:**
- Modify: `firmware/esp32/web/basic_drive.html`, `firmware/esp32/web/settings.html`
- Create: `firmware/esp32/web/test/icons_check.js`

**Interfaces:**
- Consumes: the markers and `icon()` (Tasks 1–3).

- [ ] **Step 1: Write the icons check**, `firmware/esp32/web/test/icons_check.js`:

```js
// Bridge LLD R25 browser check (Playwright run_code): the shared Lucide icons on Settings and Drive.
// Run the stand-in tank (python3 firmware/esp32/web/test/fake_tank.py 8765); it serves the packed pages (icons in).
// An icon is told apart by its parts: wifi 4, wifi-zero 1, battery-full 5, battery-low 3, plug 4.
async (page) => {
  await page.addInitScript(() => {
    class FakeWS {
      constructor(url) { this.readyState = 0; window.__ws = this; setTimeout(() => { this.readyState = 1; this.onopen && this.onopen(); }, 20); }
      send(s) { const m = JSON.parse(s); if (m.t === "hello") this.emit({ t: "welcome", you: "x-1", tank: { fw: { uno: "1.0" } } }); }
      emit(o) { setTimeout(() => this.onmessage && this.onmessage({ data: JSON.stringify(o) }), 5); }
      close() { this.readyState = 3; this.onclose && this.onclose(); }
    }
    window.WebSocket = FakeWS;
  });
  let rssi = -50;
  await page.route(/\/info$/, (r) => r.fulfill({ status: 200, contentType: "application/json", body: JSON.stringify({ net: { rssi } }) }));
  const q = (f, a) => page.evaluate(f, a);
  const parts = (sel) => q((sel) => { const g = document.querySelector(sel + " svg"); return g ? g.children.length : 0; }, sel);
  const emit = async (o) => { await q((o) => window.__ws.emit(o), o); await page.waitForTimeout(80); };
  const out = {};
  await page.setViewportSize({ width: 1280, height: 900 });
  await page.goto("http://127.0.0.1:8765/settings"); await page.waitForTimeout(600);
  await emit({ t: "wifi_status", mode: "home", rssi: -50 }); const w1 = await parts("#cWifi");
  await emit({ t: "wifi_status", mode: "home", rssi: -80 });
  out.settingsWifi = [w1, await parts("#cWifi"), await q(() => document.getElementById("cWifi").textContent.trim())];  // 4, 1, HOME
  await emit({ t: "ts", batt_v: 8.2, flags: [] }); const b1 = await parts("#cBatt");
  await emit({ t: "ts", batt_v: 7.0, flags: [] }); const b2 = await parts("#cBatt");
  await emit({ t: "ts", batt_v: 5.0, flags: [] });
  out.settingsBatt = [b1, b2, await parts("#cBatt")];                                                               // 5, 3, 4
  out.noBars = await q(() => document.querySelectorAll(".bars").length);                                           // 0
  await page.setViewportSize({ width: 844, height: 390 });
  await page.goto("http://127.0.0.1:8765/"); await page.waitForTimeout(800);
  const d1 = await parts("#wifiChip");
  rssi = -80; await q(() => window.dispatchEvent(new Event("focus"))); await page.goto("http://127.0.0.1:8765/"); await page.waitForTimeout(800);
  out.driveWifi = [d1, await parts("#wifiChip")];                                                                   // 4, 1
  return out;
}
```

- [ ] **Step 2: Run it to see it fail** (fake tank on :8765, fresh browser).
Expected: `settingsWifi` is `[0, 0, …]`: Settings draws CSS bars, not an `<svg>`. `noBars` is above 0.

- [ ] **Step 3: Replace the inline icons with markers** using this one-off script. It isn't committed; run it from `firmware/esp32/web` with the unpacked Lucide package (`npm pack lucide-static@0.460.0`, unpacked in its own folder):
  - each `<svg viewBox="0 0 24 24" …>…</svg>` in the page markup (not inside a script string) becomes `<!--icon:NAME …-->`;
  - the name is found by matching the body against Lucide;
  - hand-trimmed copies are mapped by hand.

```python
# One-off (Plan N7 Task 4 Step 3): inline icons -> <!--icon:NAME--> markers. Run in firmware/esp32/web:
#   python3 -I n7_migrate.py LUCIDE_DIR/package/icons
import pathlib, re, sys
lib = pathlib.Path(sys.argv[1])
def norm(b): return re.sub(r">\s+<", "><", re.sub(r"\s*/>", "/>", b.strip()))
L = {}
for f in lib.glob("*.svg"):
    t = f.read_text(); L.setdefault(norm(t[t.index(">", t.index("<svg")) + 1:t.rindex("</svg>")]), f.stem)
MANUAL = [("M12 3l1.9 5.8", "sparkles"), ('M12 20h.01"/><path d="M2 8.82', "wifi"), ("m22 2-7 20-4-9-9-4Z", "send"),
          ('<rect width="18" height="11" x="3" y="11" rx="2"/>', "lock"), ("M21 15v4a2", "download"), ("M2 12h20", "ruler"),
          ("M21.17 6.81", "pencil"), ("M18 11V6a2 2 0 0 0-4 0", "hand"), ("M4 6h16M4 12h16M4 18h16", "menu"),
          ("M12 5v14M5 12h14", "plus"), ("M19 21H5a2 2 0 0 1-2-2V5", "save"), ("M3 6h18M19 6v14", "trash")]
def name_of(body):
    n = L.get(norm(body))
    if n: return n
    for k, v in MANUAL:
        if k in norm(body)[:60]: return v
    raise SystemExit("unknown icon: " + body[:80])
SVG = re.compile(r'<svg viewBox="0 0 24 24"([^>]*)>(.*?)</svg>', re.S)
for page in ("basic_drive.html", "settings.html", "logo.html"):
    t = pathlib.Path(page).read_text(); count = 0
    def sub(m):
        global count
        before = t[t.rfind("\n", 0, m.start()):m.start()].rstrip()
        if before.endswith("'"): return m.group(0)  # built inside a script string: Step 4
        count += 1
        return "<!--icon:" + name_of(m.group(2)) + m.group(1) + "-->"
    new = SVG.sub(sub, t); pathlib.Path(page).write_text(new)
    print(page, count, "markers;", len(SVG.findall(new)), "svg left")
```
Expected: `basic_drive.html 20 markers; 1 svg left`, `settings.html 35 markers; 10 svg left`, `logo.html 6 markers; 0 svg left`. The ones left are the script-built icons (Step 4).

- [ ] **Step 4: The script-built icons.**
  - **Drive:** the `WIFI_ICON` table of path strings becomes:
    ```js
      /*CQ:ICONS wifi wifi-high wifi-low wifi-zero wifi-off*/  // the shared Lucide icons (bridge LLD R25)
      var WIFI_ICON = { full: "wifi", high: "wifi-high", low: "wifi-low", zero: "wifi-zero", off: "wifi-off" };
    ```
    and `showWifi`'s first line becomes `    $("wifiChip").innerHTML = icon(WIFI_ICON[k]);`.
  - **Settings:**
    - right after the main script's `"use strict";` add:
      ```js
        /*CQ:ICONS lock pencil trash hand triangle-alert wifi wifi-high wifi-low wifi-zero rotate-ccw info check battery-full battery-medium battery-low battery-warning plug*/  // the shared Lucide icons (bridge LLD R25)
      ```
    - each `var SVG_X = '<svg …>';` becomes `var SVG_X = icon("NAME");`, with: `SVG_LOCK` lock, `SVG_EDIT` pencil, `SVG_DEL` trash, `SVG_HAND` hand, `SVG_WARN` triangle-alert, `SVG_WIFI` wifi, `SVG_REST` rotate-ccw, `SVG_INFO` info, `SVG_OK` check;
    - `var SVG_BATT …` is removed;
    - `bars(rssi)` becomes (and `battIcon` is added after it):
      ```js
        function bars(rssi) {  // the WiFi strength as Drive shows it (bridge LLD R25): markup only from numbers
          return icon(rssi > -55 ? "wifi" : rssi > -65 ? "wifi-high" : rssi > -75 ? "wifi-low" : "wifi-zero");
        }
        function battIcon(v) {  // 2S Li-ion, Drive's charge curve: >= 70 % / 35 % / 10 % (bridge LLD R25)
          return icon(v < 5.5 ? "plug" : v >= 7.85 ? "battery-full" : v >= 7.4 ? "battery-medium" : v >= 7.0 ? "battery-low" : "battery-warning");
        }
      ```
    - `showBatt`: `$("cBatt").innerHTML = battIcon(v) + " " + v.toFixed(2) + " V";`;
    - the four `.bars …` CSS rules are removed.

- [ ] **Step 5: Compare screenshots, before and after.**
  - Serve the original pages with `python3 -m http.server 8766` in `main`'s `firmware/esp32/web`, and the packed new ones from the fake tank on :8765.
  - Shoot each view in both, under the same frozen state: a WebSocket that never connects, no `fetch` answers, a frozen `Date.now`, animations off.
  - The views:
    - Drive at 1280 × 800 and 844 × 390, plus its ☰ menu;
    - Settings TANK / WIFI / TUNE / LOG at 1280 × 900.
  - Compare the PNG bytes.
  - First, shoot the original against itself to prove it's deterministic (all `true`).

  Expected:
  - pixel-identical: Drive wide, Drive phone, Settings TANK, Settings LOG;
  - one icon different in each of three views. Crop and look at each:
    - Drive menu "AI": the hand-drawn star → `sparkles`;
    - Settings WIFI "Save & connect": the older paper plane → `send`, nearly identical;
    - Settings TUNE "distance wizard": the hand-drawn tick line → `ruler`.

- [ ] **Step 6: Run all the checks** (fake tank restarted, each in a fresh browser).
Expected:
- `icons_check`: `{"settingsWifi":[4,1,"HOME"],"settingsBatt":[5,3,4],"noBars":0,"driveWifi":[4,1]}`;
- both camera checks as in Task 3 Step 5;
- packer tests 12/12.
- `python3 scripts/pack_web.py`: `/ 20175 B`, `/settings 22523 B`.

- [ ] **Step 7: Commit**

```bash
git add firmware/esp32/web/basic_drive.html firmware/esp32/web/settings.html firmware/esp32/web/test/icons_check.js
git commit -m "feat(web): Drive and Settings icons from the shared Lucide file; Settings WiFi and battery as Drive's (bridge LLD R25)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: Logo's icons

**Files:**
- Modify: `firmware/esp32/web/logo.js`, `firmware/esp32/web/logo.html`, `firmware/esp32/web/test/logo.test.js`, `firmware/esp32/web/test/browser_check_wide.js`

**Interfaces:**
- Produces: `LogoCore.wifiIcon(rssi) -> name`, `LogoCore.batteryIcon(v) -> name`.

- [ ] **Step 1: Node tests** (append to `test/logo.test.js`):

```js
// ---------- revision N7: the shared Lucide icons (bridge LLD R25, Tank Logo LLD §23.9) ----------
test("the WiFi icon follows the signal as on the Drive page", () => {
  assert.equal(L.wifiIcon(-50), "wifi");
  assert.equal(L.wifiIcon(-60), "wifi-high");
  assert.equal(L.wifiIcon(-70), "wifi-low");
  assert.equal(L.wifiIcon(-80), "wifi-zero");
  assert.equal(L.wifiIcon(null), "wifi-off");
});

test("the battery icon follows the charge, and USB is a plug", () => {
  assert.equal(L.batteryIcon(8.2), "battery-full");
  assert.equal(L.batteryIcon(7.6), "battery-medium");
  assert.equal(L.batteryIcon(7.1), "battery-low");
  assert.equal(L.batteryIcon(6.8), "battery-warning");
  assert.equal(L.batteryIcon(5.0), "plug");
});
```

- [ ] **Step 2: Run them to see them fail**

Run: `node --test firmware/esp32/web/test/logo.test.js 2>&1 | grep -E "^ℹ (pass|fail)"`
Expected: `pass 52`, `fail 2`.

- [ ] **Step 3: `logo.js`.** Apply:

```diff
diff --git a/firmware/esp32/web/logo.js b/firmware/esp32/web/logo.js
index 08b433c..b15e3f5 100644
--- a/firmware/esp32/web/logo.js
+++ b/firmware/esp32/web/logo.js
@@ -299,11 +299,21 @@ var LogoCore = (function () {
   // The camera streams only while it can be seen.
   function cameraWanted(wide, hiddenByUser, pageHidden) { return wide && !hiddenByUser && !pageHidden; }
 
+  // ---------- revision N7: the shared Lucide icons (bridge LLD R25) ----------
+  // The Drive page's rules: WiFi by RSSI; a 2S battery by its charge curve (>= 70 % / 35 % / 10 %); under 5.5 V it is USB.
+  function wifiIcon(rssi) {
+    if (rssi === null || rssi === undefined) return "wifi-off";
+    return rssi > -55 ? "wifi" : rssi > -65 ? "wifi-high" : rssi > -75 ? "wifi-low" : "wifi-zero";
+  }
+  function batteryIcon(v) {
+    return v < 5.5 ? "plug" : v >= 7.85 ? "battery-full" : v >= 7.4 ? "battery-medium" : v >= 7.0 ? "battery-low" : "battery-warning";
+  }
+
   return {
     wordIndex: wordIndex, ownWords: ownWords, colour: colour, insertWord: insertWord, Drafts: Drafts,
     fitView: fitView, toScreen: toScreen, plannedRuns: plannedRuns, CheckScheduler: CheckScheduler, RequestQueue: RequestQueue,
     runBarStart: runBarStart, runBar: runBar, canRun: canRun, locked: locked, endText: endText, NACK_TEXT: NACK_TEXT,
-    shortcut: shortcut, panelTab: panelTab, cameraWanted: cameraWanted
+    shortcut: shortcut, panelTab: panelTab, cameraWanted: cameraWanted, wifiIcon: wifiIcon, batteryIcon: batteryIcon
   };
 })();
 if (typeof module !== "undefined") module.exports = LogoCore;
```

- [ ] **Step 4: The laptop check's new lines.** Apply:

```diff
diff --git a/firmware/esp32/web/test/browser_check_wide.js b/firmware/esp32/web/test/browser_check_wide.js
index b73d59b..a98d621 100644
--- a/firmware/esp32/web/test/browser_check_wide.js
+++ b/firmware/esp32/web/test/browser_check_wide.js
@@ -78,7 +78,8 @@ async (page) => {
   await page.click("#camShow"); await page.waitForTimeout(300);
   out.camBack = [await q(() => /:81\/stream/.test(document.getElementById("camImg").src)), streams > before];  // true, true
   await q(() => window.__fakeEmit({ t: "tf", dist_cm: 64 })); await page.waitForTimeout(50);
-  out.sonar = await q(() => document.getElementById("camDist").textContent);                                // ▸ 64 cm
+  out.sonar = [await q(() => document.getElementById("camDistT").textContent),                           // 64 cm
+    await q(() => !!document.querySelector("#camDist svg"))];                                               // true (radar)
   // final review: held keys are swallowed (no browser "save page" / F10 menu) but never act twice
   const key = (k, o) => q(([k, o]) => !window.dispatchEvent(new KeyboardEvent("keydown", Object.assign({ key: k, cancelable: true }, o))), [k, o]);
   const runs = await sent("logo_run");
@@ -106,6 +107,16 @@ async (page) => {
       return b("#cPath .stats").bottom <= b("#cPath").bottom + 0.5 && b("#cPath").bottom <= b("#bar").top + 0.5 &&
         document.scrollingElement.scrollHeight <= innerHeight; });                                           // true
   }
+  // N7: the top-bar icons are the shared Lucide ones (bridge LLD R25): their parts tell them apart
+  const parts = (id) => q((id) => { const g = document.querySelector("#" + id + " svg"); return g ? g.children.length : 0; }, id);
+  const emit = async (o) => { await q((o) => window.__fakeEmit(o), o); await page.waitForTimeout(60); };
+  await emit({ t: "wifi_status", mode: "home", rssi: -50 }); const wFull = await parts("cWifi");
+  await emit({ t: "wifi_status", mode: "home", rssi: -80 });
+  out.wifiIcon = [wFull, await parts("cWifi"), await q(() => document.getElementById("cWifi").textContent.trim())];  // 4, 1, HOME
+  await emit({ t: "ts", batt_v: 8.2, flags: [] }); const bFull = await parts("cBatt");
+  await emit({ t: "ts", batt_v: 7.0, flags: [] }); const bLow = await parts("cBatt");
+  await emit({ t: "ts", batt_v: 5.0, flags: [] });
+  out.battIcon = [bFull, bLow, await q(() => document.getElementById("cBatt").textContent.trim()), await parts("cUno")];  // 5, 3, USB, >0
   // narrow again: the phone layout, no camera stream, the problems back under the editor
   await page.setViewportSize({ width: 390, height: 844 }); await page.waitForTimeout(300);
   out.phone = [await q(() => document.getElementById("camImg").hasAttribute("src")),        // false
```
Run it (fresh browser). Expected: it fails at `camDistT` (no such element yet).

- [ ] **Step 5: `logo.html`.** Apply:

```diff
diff --git a/firmware/esp32/web/logo.html b/firmware/esp32/web/logo.html
index 8e5ad21..bc44766 100644
--- a/firmware/esp32/web/logo.html
+++ b/firmware/esp32/web/logo.html
@@ -124,15 +124,15 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
 <body>
 <header><div class="inner">
   <div class="top">
-    <a href="/"><svg viewBox="0 0 24 24"><path d="m15 18-6-6 6-6"/></svg>DRIVE</a>
+    <a href="/"><!--icon:chevron-left-->DRIVE</a>
     <h1>[ TANK LOGO ]</h1>
-    <button id="mbtn" type="button" aria-label="Menu"><svg viewBox="0 0 24 24"><path d="M4 6h16M4 12h16M4 18h16"/></svg></button>
-    <button id="stop" type="button">STOP</button>
+    <button id="mbtn" type="button" aria-label="Menu"><!--icon:menu--></button>
+    <button id="stop" type="button"><!--icon:octagon-x--> STOP</button>
   </div>
   <div class="chips">
-    <span class="chip" id="cBatt">-- V</span>
-    <span class="chip" id="cWifi">WIFI --</span>
-    <span class="chip" id="cUno">● UNO --</span>
+    <span class="chip" id="cBatt"><!--icon:battery--> -- V</span>
+    <span class="chip" id="cWifi"><!--icon:wifi-off--> --</span>
+    <span class="chip" id="cUno"><!--icon:cpu--> UNO --</span>
   </div>
 </div></header>
 <nav id="menu"><a href="/">DRIVE</a><a href="/settings">SETTINGS</a><a href="/help">HELP</a></nav>
@@ -141,10 +141,10 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
   <h2>[ PROGRAMS ]</h2>
   <div class="progbar">
     <select id="psel" aria-label="Program"></select>
-    <button id="pnew" type="button" title="New program"><svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg><span> NEW</span></button>
-    <button id="psave" type="button" title="Save on the tank"><svg viewBox="0 0 24 24"><path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/><path d="M17 21v-8H7v8M7 3v5h8"/></svg><span> SAVE</span></button>
-    <button id="pren" type="button" title="Rename"><svg viewBox="0 0 24 24"><path d="M21.17 6.81a1 1 0 0 0-3.99-3.99L3.84 16.17a2 2 0 0 0-.5.83l-1.32 4.35a.5.5 0 0 0 .62.62l4.35-1.32a2 2 0 0 0 .83-.5z"/></svg><span> RENAME</span></button>
-    <button id="pdel" type="button" title="Delete" style="color:var(--danger)"><svg viewBox="0 0 24 24"><path d="M3 6h18M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg><span> DELETE</span></button>
+    <button id="pnew" type="button" title="New program"><!--icon:plus--><span> NEW</span></button>
+    <button id="psave" type="button" title="Save on the tank"><!--icon:save--><span> SAVE</span></button>
+    <button id="pren" type="button" title="Rename"><!--icon:pencil--><span> RENAME</span></button>
+    <button id="pdel" type="button" title="Delete" style="color:var(--danger)"><!--icon:trash--><span> DELETE</span></button>
   </div>
   <ul id="plist"></ul>
 </section>
@@ -164,8 +164,8 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
   <div class="msgs"><div id="log"></div><canvas id="mini" title="Open the path"></canvas></div>
 </section>
 <section class="card" id="cPath">
-  <h2>[ 02 // PATH ]<span><button id="camShow" type="button">SHOW CAMERA</button> <span id="pgrid" class="badge">5 cm GRID</span></span></h2>
-  <div class="cam" id="cam"><img id="camImg" alt=""><span id="camNote">NO CAMERA</span><b id="camDist">&#9656; -- cm</b><button id="camHide" type="button">HIDE</button></div>
+  <h2>[ 02 // PATH ]<span><button id="camShow" type="button"><!--icon:camera--> SHOW CAMERA</button> <span id="pgrid" class="badge">5 cm GRID</span></span></h2>
+  <div class="cam" id="cam"><img id="camImg" alt=""><span id="camNote">NO CAMERA</span><b id="camDist"><!--icon:radar--> <span id="camDistT">-- cm</span></b><button id="camHide" type="button"><!--icon:eye-off--> HIDE</button></div>
   <div class="pathw"><canvas id="path"></canvas><span id="pnote"></span></div>
   <div class="stats">
     <div><small>HEADING</small><b id="sHead">--</b></div>
@@ -175,8 +175,8 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
 </section>
 <div id="bar">
   <span id="status">● CONNECTING</span>
-  <button id="step" type="button">STEP</button>
-  <button id="run" type="button">▶ RUN</button>
+  <button id="step" type="button"><!--icon:step-forward--> STEP</button>
+  <button id="run" type="button"><!--icon:play--> <span id="runT">RUN</span></button>
 </div>
 </main>
 <div class="sheet" id="nameS"><div class="card">
@@ -374,7 +374,7 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
     if (!open && !L.locked(s)) { t = "● NOT CONNECTED"; cls = "s-warn"; }
     st.textContent = t; st.className = cls;
     $("run").disabled = !(L.canRun(s) || s.st === "stepping") || !open;
-    $("run").textContent = s.st === "stepping" ? "▶ NEXT" : "▶ RUN";
+    $("runT").textContent = s.st === "stepping" ? "NEXT" : "RUN";
     $("step").disabled = !(L.canRun(s) || s.st === "stepping") || !open;
     $("state").textContent = L.locked(s) ? "RUNNING" : s.st === "ended" ? "ENDED" : "";
     $("state").className = "badge" + ($("state").textContent ? "" : " hide");
@@ -607,20 +607,23 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
   $("mbtn").addEventListener("click", function () { $("menu").classList.toggle("on"); });
 
   // ---------- tank link (bridge R11: no link while the page is hidden) ----------
-  function bars(rssi) { return rssi >= -55 ? "▂▄▆█" : rssi >= -65 ? "▂▄▆" : rssi >= -75 ? "▂▄" : "▂"; }
+  /*CQ:ICONS wifi wifi-high wifi-low wifi-zero wifi-off battery-full battery-medium battery-low battery-warning plug cpu*/  // R25
+  function chip(id, name, text, warn) { $(id).innerHTML = icon(name) + " " + esc(text); $(id).className = "chip" + (warn ? " warn" : ""); }
   function onMessage(m) {
     switch (m.t) {
-      case "welcome": $("cUno").textContent = m.tank && m.tank.fw && m.tank.fw.uno ? "● UNO ONLINE" : "● UNO OFFLINE"; break;
+      case "welcome": chip("cUno", "cpu", m.tank && m.tank.fw && m.tank.fw.uno ? "UNO ONLINE" : "UNO OFFLINE", false); break;
       case "ts":
-        if (m.batt_v !== null && m.batt_v !== undefined) $("cBatt").textContent = m.batt_v < 5.5 ? "USB" : m.batt_v.toFixed(2) + " V";
+        if (m.batt_v !== null && m.batt_v !== undefined) chip("cBatt", L.batteryIcon(m.batt_v), m.batt_v < 5.5 ? "USB" : m.batt_v.toFixed(2) + " V", m.batt_v >= 5.5 && m.batt_v < 7.0);
         var off = (m.flags || []).indexOf("body_offline") >= 0;
-        $("cUno").textContent = off ? "● UNO OFFLINE" : "● UNO ONLINE"; $("cUno").className = "chip" + (off ? " warn" : "");
+        chip("cUno", "cpu", off ? "UNO OFFLINE" : "UNO ONLINE", off);
         break;
       case "tf":
         $("sDist").textContent = m.dist_cm === null || m.dist_cm === undefined ? "--" : m.dist_cm + " cm";
-        $("camDist").textContent = "\u25B8 " + $("sDist").textContent;
+        $("camDistT").textContent = $("sDist").textContent;
+        break;
+      case "wifi_status":
+        chip("cWifi", m.mode === "home" ? L.wifiIcon(m.rssi) : "wifi", m.mode === "home" ? "HOME" : "OWN WIFI", m.mode === "home" && m.rssi <= -65);
         break;
-      case "wifi_status": $("cWifi").textContent = m.mode === "home" ? "HOME " + bars(m.rssi) : "OWN WIFI"; $("cWifi").className = "chip"; break;
       case "ev": if (m.kind && m.kind.indexOf("logo_") === 0) onEvent(m); break;
       case "ack": if (m.id === runId) { onBar({ e: "ack" }); lockEditor(); } break;
       case "nack":
@@ -641,7 +644,7 @@ main { padding: 10px 12px; display: grid; gap: 10px; }
     };
     ws.onmessage = function (e) { var m; try { m = JSON.parse(e.data); } catch (x) { return; } onMessage(m); };
     ws.onclose = function () {
-      open = false; $("cWifi").textContent = "RECONNECTING"; $("cWifi").className = "chip warn";
+      open = false; chip("cWifi", "wifi-off", "RECONNECTING", true);
       onBar({ e: "closed" }); lockEditor();
       setTimeout(connect, retry); retry = Math.min(retry * 2, 5000);
     };
```

- [ ] **Step 6: Run everything** (pack, restart the fake tank, a fresh browser per check).
Expected:
- Node 54/54.
- `n6_wide`: all N6 lines as before, plus:
  - `sonar ["64 cm", true]`;
  - `wifiIcon [4, 1, "HOME"]`;
  - `battIcon [5, 3, "USB", 10]`.
- `n4_phone`: unchanged.
- `/logo` 24 514 B (budget 26 KB).

- [ ] **Step 7: Commit**

```bash
git add firmware/esp32/web/logo.js firmware/esp32/web/logo.html firmware/esp32/web/test/logo.test.js firmware/esp32/web/test/browser_check_wide.js
git commit -m "feat(logo): Lucide icons - WiFi, battery, Uno, RUN / STEP / STOP, camera and sonar (Tank Logo LLD §23.9)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 6: On the tank (owner)

- [ ] **Step 1:** Build and flash the bridge over WiFi, as Plan N6 Task 4 did (OTA password never printed; `secrets.h` copied in and deleted afterwards). Nothing moves.
- [ ] **Step 2: The owner looks at each page:**
  - **Drive:** the WiFi icon and the ☰ menu (AI = sparkles).
  - **Settings:**
    - the top-bar WiFi and battery icons;
    - the WiFi tab's network list (signal icons);
    - the Log tab's icons;
    - the distance wizard's ruler.
  - **Logo:** the WiFi / battery / Uno chips update, RUN / STEP / STOP icons, HIDE / SHOW, and the sonar on the picture.
  - The battery, WiFi and sonar readouts must show real values (design-notes §149's open item).

### Task 7: Docs as built
- [ ] **Step 1:** Bridge LLD R25.5 "As built": files, numbers, rulings, the tank look.
- [ ] **Step 2:** Design-notes §150.
- [ ] **Step 3:** `CLAUDE.md`: the plans list and the order line.
- [ ] **Step 4:** Commit and push. Merge only when the owner says so.

## Rulings made while drafting (against R25)
1. **Settings also gets Drive's Lucide WiFi-strength and battery icons** (its CSS bars and hand-drawn battery go). R25 said Settings would "look the same", but the owner's complaint (non-standard icons) applies there too. Cost if wrong: put the bars back.
2. **Three hand-drawn icons became the nearest Lucide ones:** the AI star → `sparkles`, the distance wizard's tick line → `ruler` (not in Lucide), and the older `send`.
3. **DELETE on the Logo page uses `trash`** (as Settings), not `trash-2` as §23.9 said, so one trash icon is used everywhere.
4. **`pack_web.py` finds `icons.py` without `__file__`** when SCons runs it (it uses the project directory).
5. **The two Settings camera checks now load the packed page from `fake_tank.py`.** Before, they loaded the raw file through `http.server`.
6. **Browser checks run each in a fresh browser.** The screenshot script's frozen clock and dead `fetch` stayed on the tab and broke the next check until the browser was closed.

