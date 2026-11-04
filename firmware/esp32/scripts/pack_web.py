"""PlatformIO pre-script: web assets -> build/web_assets.h (bridge LLD R2.3 step 5).

- web/basic_drive.html (served at /), web/manifest.webmanifest: gzip -9, served with
  Content-Encoding: gzip. Budget: the page must stay <= 25 KB gzipped (LLD R2.12).
- /icon-192.png: drawn here (no binary file in git), stored as PNG (already compressed).
- /help: web/help.html with <!--CQ:PROTOCOL--> replaced by the protogen output
  src/generated/help_protocol.html and <!--CQ:LOGO--> by web/help_logo.html (Task 9);
  skipped while web/help.html does not exist.
- /ai (Plan Z2b-1, bridge LLD R27.4): web/ai.html with <!--CQ:AI_JS--> replaced by web/ai.js. Budget 24 KB gzipped.
- /logo (Plan N4, Tank Logo LLD §21): web/logo.html with <!--CQ:LOGO_JS--> replaced by
  web/logo.js and <!--CQ:WORDS--> by web/generated/words.json. Budget 32 KB gzipped (bridge LLD R25, R28, R29, R30).
- Every page: the shared look (bridge LLD R28) - <!--CQ:THEME--> in its <head> becomes web/theme.css and web/theme.js.
- Every page: the one top bar (bridge LLD R29) - <!--CQ:BAR--> becomes web/bar.css, web/bar.html and web/bar.js.
- /ai, /logo, /settings: the status strip (bridge LLD R30) - <!--CQ:STRIP--> becomes web/strip.css, strip.html, and
  tank_model.js + strip.js (R31: the model and its view).
- /, /ai, /logo, /settings: the one link (bridge LLD R31) - <!--CQ:LINK--> becomes web/link.js.
- Every page: whole-line // comments in its scripts and /* */ in its styles go (bridge LLD R31.5).
- Every page: its icons from web/icons.json (scripts/icons.py, bridge LLD R25) - <!--icon:NAME--> markers and one
  /*CQ:ICONS ...*/ table; an unknown icon fails the build.
Also runnable alone: python3 scripts/pack_web.py [project_dir]
"""

from __future__ import annotations

import gzip
import pathlib
import re
import struct
import sys
import zlib

# PlatformIO runs this file through SCons, which removes __file__ before running it. SCons puts this folder first on
# sys.path anyway, and PlatformIO keeps the project as the working directory (SConscriptChdir(False)), so
# cwd/scripts is the same folder.
_HERE = pathlib.Path(__file__).resolve().parent if "__file__" in globals() else pathlib.Path.cwd() / "scripts"
sys.path.insert(0, str(_HERE))
import cq_icons as icons

PAGE_BUDGET = 25 * 1024
HELP_BUDGET = 16 * 1024
LOGO_BUDGET = 32 * 1024  # Plan N4 (Tank Logo LLD §21.9): /logo, gzipped, words included; R25: 26 KB with icons; R28: 28 KB with the shared look; R29: 30 KB with the top bar; R30: 32 KB with the status strip (owner)
SETTINGS_BUDGET = 36 * 1024  # Plan M: /settings, gzipped; Plan O (LLD R13.1): 36 KB for four tabs
AI_BUDGET = 24 * 1024  # Plan Z2b-1 (bridge LLD R27.4, design-notes §200): /ai, gzipped, ai.js and icons in


def png_icon(size: int = 192) -> bytes:
    """Phosphor icon: near-black square, green ring, green forward chevron."""
    bg, green, dim = (5, 8, 5), (51, 255, 102), (31, 154, 64)
    c = (size - 1) / 2
    rows = bytearray()
    for y in range(size):
        rows.append(0)  # filter: none
        for x in range(size):
            dx, dy = x - c, y - c
            r = (dx * dx + dy * dy) ** 0.5
            px = bg
            if 0.36 * size <= r <= 0.43 * size:
                px = green
            elif r < 0.30 * size:
                # chevron pointing up: two strokes from the tip
                u = (y - 0.25 * size) / (0.40 * size)  # 0 at the tip, 1 at the bottom
                if 0 <= u <= 1 and abs(abs(dx) - u * 0.26 * size) < 0.06 * size:
                    px = green
                elif r < 0.05 * size:
                    px = dim
            rows += bytes(px)

    def chunk(kind: bytes, data: bytes) -> bytes:
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )


def gz(data: bytes) -> bytes:
    return gzip.compress(data, compresslevel=9, mtime=0)  # mtime 0: reproducible builds


def assemble_help(project: pathlib.Path) -> bytes | None:
    page = project / "web" / "help.html"
    if not page.exists():
        return None
    text = page.read_text(encoding="utf-8")
    proto = (project / "src" / "generated" / "help_protocol.html").read_text(encoding="utf-8")
    logo = (project / "web" / "help_logo.html").read_text(encoding="utf-8")
    for marker in ("<!--CQ:PROTOCOL-->", "<!--CQ:LOGO-->"):
        if marker not in text:
            raise SystemExit(f"pack_web: {marker} missing in web/help.html")
    return text.replace("<!--CQ:PROTOCOL-->", proto).replace("<!--CQ:LOGO-->", logo).encode()


def assemble_logo(project: pathlib.Path) -> bytes:
    web = project / "web"
    text = (web / "logo.html").read_text(encoding="utf-8")
    for marker in ("<!--CQ:LOGO_JS-->", "<!--CQ:WORDS-->"):
        if marker not in text:
            raise SystemExit(f"pack_web: {marker} missing in web/logo.html")
    code = (web / "logo.js").read_text(encoding="utf-8").replace("</", "<\\/")  # never closes its <script>
    words = (web / "generated" / "words.json").read_text(encoding="utf-8").strip()
    words = words.replace("</", "<\\/")  # never closes the <script> it sits in
    return text.replace("<!--CQ:LOGO_JS-->", code).replace("<!--CQ:WORDS-->", words).encode()


def assemble_drive(project: pathlib.Path) -> bytes:
    """/ (bridge LLD R31): web/basic_drive.html with <!--CQ:DRIVE_JS--> replaced by web/drive.js (DriveModel)."""
    web = project / "web"
    text = (web / "basic_drive.html").read_text(encoding="utf-8")
    if "<!--CQ:DRIVE_JS-->" not in text:
        raise SystemExit("pack_web: <!--CQ:DRIVE_JS--> missing in web/basic_drive.html")
    code = (web / "drive.js").read_text(encoding="utf-8").replace("</", "<\\/")  # never closes its <script>
    return text.replace("<!--CQ:DRIVE_JS-->", code).encode()


def assemble_ai(project: pathlib.Path) -> bytes:
    """/ai (Plan Z2b-1, bridge LLD R27.4): web/ai.html with <!--CQ:AI_JS--> replaced by web/ai.js."""
    web = project / "web"
    text = (web / "ai.html").read_text(encoding="utf-8")
    if "<!--CQ:AI_JS-->" not in text:
        raise SystemExit("pack_web: <!--CQ:AI_JS--> missing in web/ai.html")
    code = (web / "ai.js").read_text(encoding="utf-8").replace("</", "<\\/")  # never closes its <script>
    return text.replace("<!--CQ:AI_JS-->", code).encode()


THEME_MARKER = "<!--CQ:THEME-->"


def with_theme(text: str, project: pathlib.Path, page: str) -> str:
    """The shared look (bridge LLD R28): THEME_MARKER becomes <style>theme.css</style><script>theme.js</script>."""
    if THEME_MARKER not in text:
        raise SystemExit(f"pack_web: {THEME_MARKER} missing in {page}")
    web = project / "web"
    css = theme_css_for((web / "theme.css").read_text(encoding="utf-8"), page)
    if "</style" in css.lower():
        raise SystemExit("pack_web: web/theme.css contains </style, which would end the page's <style> early")
    code = (web / "theme.js").read_text(encoding="utf-8").replace("</", "<\\/")  # never closes its <script>
    return text.replace(THEME_MARKER, f"<style>\n{css}</style>\n<script>\n{code}</script>", 1)


BAR_MARKER = "<!--CQ:BAR-->"


def with_bar(text: str, project: pathlib.Path, page: str) -> str:
    """The one top bar (bridge LLD R29): BAR_MARKER becomes <style>bar.css</style> + bar.html + <script>bar.js</script>.
    A page without the marker fails the build (every page has the bar)."""
    if BAR_MARKER not in text:
        raise SystemExit(f"pack_web: {BAR_MARKER} missing in {page}")
    web = project / "web"
    css = (web / "bar.css").read_text(encoding="utf-8")
    if "</style" in css.lower():
        raise SystemExit("pack_web: web/bar.css contains </style, which would end its <style> early")
    markup = (web / "bar.html").read_text(encoding="utf-8")
    code = (web / "bar.js").read_text(encoding="utf-8").replace("</", "<\\/")  # never closes its <script>
    return text.replace(BAR_MARKER, f"<style>\n{css}</style>\n{markup}<script>\n{code}</script>", 1)


STRIP_MARKER = "<!--CQ:STRIP-->"


def with_strip(text: str, project: pathlib.Path, page: str) -> str:
    """The status strip (bridge LLD R30): STRIP_MARKER becomes <style>strip.css</style> + strip.html + <script>strip.js</script>.
    A page without the marker is left as it is (only /ai, /logo and /settings carry it) - unless it uses cqStrip."""
    if STRIP_MARKER not in text:
        if "cqStrip." in text:
            raise SystemExit(f"pack_web: {page} uses cqStrip but has no {STRIP_MARKER}")
        return text
    web = project / "web"
    css = (web / "strip.css").read_text(encoding="utf-8")
    if "</style" in css.lower():
        raise SystemExit("pack_web: web/strip.css contains </style, which would end its <style> early")
    markup = (web / "strip.html").read_text(encoding="utf-8")
    code = ((web / "tank_model.js").read_text(encoding="utf-8") + (web / "strip.js").read_text(encoding="utf-8")).replace(
        "</", "<\\/")  # the model (R31) then its view; never closes its <script>
    return text.replace(STRIP_MARKER, f"<style>\n{css}</style>\n{markup}<script>\n{code}</script>", 1)


LINK_MARKER = "<!--CQ:LINK-->"


def with_link(text: str, project: pathlib.Path, page: str) -> str:
    """The one link (bridge LLD R31): LINK_MARKER becomes <script>link.js</script>. A page that uses cqLink without the
    marker fails the build; a page without either is left as it is (/help holds no link)."""
    if LINK_MARKER not in text:
        if "cqLink." in text:
            raise SystemExit(f"pack_web: {page} uses cqLink but has no {LINK_MARKER}")
        return text
    code = (project / "web" / "link.js").read_text(encoding="utf-8").replace("</", "<\\/")  # never closes its <script>
    return text.replace(LINK_MARKER, f"<script>\n{code}</script>", 1)


PAGES_LINE = re.compile(r"^/\* pages: ([^*]*)\*/\n", re.MULTILINE)


def theme_css_for(css: str, page: str) -> str:
    """theme.css as one page gets it: the part before the first "/* pages: ... */" line goes to every page; each
    section after such a line only to the pages it names (each page carries only its own Sunny values)."""
    parts = PAGES_LINE.split(css)
    out = [parts[0]]
    for pages, body in zip(parts[1::2], parts[2::2]):
        if page in pages.split():
            out.append(body)
    return "".join(out)


SCRIPT_BLOCK = re.compile(r"(<script>)(.*?)(</script>)", re.DOTALL)
STYLE_BLOCK = re.compile(r"(<style>)(.*?)(</style>)", re.DOTALL)
LINE_COMMENT = re.compile(r"^[ \t]*//[^\n]*\n", re.MULTILINE)
CSS_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(text: str) -> str:
    """Bridge LLD R31.5: whole-line // comments inside <script> and /* */ inside <style> go; strings, trailing comments
    and everything outside those blocks (Help's <pre>) stay. Run last, after the parts and icons have read theirs."""
    text = SCRIPT_BLOCK.sub(lambda m: m.group(1) + LINE_COMMENT.sub("", m.group(2)) + m.group(3), text)
    return STYLE_BLOCK.sub(lambda m: m.group(1) + CSS_COMMENT.sub("", m.group(2)) + m.group(3), text)


def assemble_page(project: pathlib.Path, uri: str, keep_comments: bool = False) -> bytes:
    """A page as served (before gzip), its icons put in (bridge LLD R25), its comments out (R31.5). /help may be
    missing: b"". keep_comments: the page as written, for the size test."""
    web = project / "web"
    if uri == "/":
        raw = assemble_drive(project)
    elif uri == "/settings":
        raw = (web / "settings.html").read_bytes()
    elif uri == "/logo":
        raw = assemble_logo(project)
    elif uri == "/ai":
        raw = assemble_ai(project)
    else:
        raw = assemble_help(project) or b""
    if not raw:
        return raw
    table = icons.load(web / "icons.json")
    text = with_link(with_strip(with_bar(with_theme(raw.decode("utf-8"), project, uri), project, uri), project, uri), project, uri)
    text = icons.expand(text, table, uri)
    return (text if keep_comments else strip_comments(text)).encode("utf-8")


def c_array(name: str, data: bytes) -> str:
    lines = [f"inline const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 20):
        lines.append("  " + ", ".join(f"0x{b:02x}" for b in data[i : i + 20]) + ",")
    lines.append("};")
    return "\n".join(lines)


def build(project: pathlib.Path) -> None:
    web = project / "web"
    # (url, content type, data, gzipped)
    assets: list[tuple[str, str, bytes, bool]] = [
        ("/", "text/html; charset=utf-8", gz(assemble_page(project, "/")), True),
        (
            "/manifest.webmanifest",
            "application/manifest+json",
            gz((web / "manifest.webmanifest").read_bytes()),
            True,
        ),
        ("/icon-192.png", "image/png", png_icon(), False),
    ]
    if len(assets[0][2]) > PAGE_BUDGET:
        raise SystemExit(f"pack_web: page {len(assets[0][2])} B gzipped > {PAGE_BUDGET} B budget")
    settings = gz(assemble_page(project, "/settings"))  # Plan M (spec 02 Part I)
    if len(settings) > SETTINGS_BUDGET:
        raise SystemExit(f"pack_web: /settings {len(settings)} B gzipped > {SETTINGS_BUDGET} B budget")
    assets.append(("/settings", "text/html; charset=utf-8", settings, True))
    logo = gz(assemble_page(project, "/logo"))
    if len(logo) > LOGO_BUDGET:
        raise SystemExit(f"pack_web: /logo {len(logo)} B gzipped > {LOGO_BUDGET} B budget")
    assets.append(("/logo", "text/html; charset=utf-8", logo, True))
    ai = gz(assemble_page(project, "/ai"))  # Plan Z2b-1 (bridge LLD R27.4)
    if len(ai) > AI_BUDGET:
        raise SystemExit(f"pack_web: /ai {len(ai)} B gzipped > {AI_BUDGET} B budget")
    assets.append(("/ai", "text/html; charset=utf-8", ai, True))
    help_page = assemble_page(project, "/help") if (web / "help.html").exists() else None
    if help_page is not None:
        packed = gz(help_page)
        if len(packed) > HELP_BUDGET:
            raise SystemExit(f"pack_web: /help {len(packed)} B gzipped > {HELP_BUDGET} B budget")
        assets.append(("/help", "text/html; charset=utf-8", packed, True))
    out = [
        "// generated by scripts/pack_web.py - do not edit, git-ignored",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "namespace cq {",
        "namespace web {",
    ]
    for i, (_, _, data, _) in enumerate(assets):
        out.append(c_array(f"kAsset{i}", data))
    out += [
        "struct Asset {",
        "  const char* uri;",
        "  const char* type;",
        "  const uint8_t* data;",
        "  size_t len;",
        "  bool gzip;",
        "};",
        "inline const Asset kAssets[] = {",
    ]
    for i, (uri, ctype, _, zipped) in enumerate(assets):
        flag = "true" if zipped else "false"
        out.append(f'  {{"{uri}", "{ctype}", kAsset{i}, sizeof(kAsset{i}), {flag}}},')
    out += [
        "};",
        "constexpr size_t kAssetCount = sizeof(kAssets) / sizeof(kAssets[0]);",
        "}  // namespace web",
        "}  // namespace cq",
        "",
    ]
    target = project / "build" / "web_assets.h"
    target.parent.mkdir(exist_ok=True)
    target.write_text("\n".join(out), encoding="utf-8")
    sizes = ", ".join(f"{uri} {len(data)} B" for uri, _, data, _ in assets)
    print(f"pack_web: {sizes}")


if __name__ == "__main__":
    build(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve())
elif "Import" in globals() or hasattr(__import__("builtins"), "Import"):  # PlatformIO script
    Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env
    build(pathlib.Path(env.subst("$PROJECT_DIR")))  # type: ignore[name-defined]  # noqa: F821
