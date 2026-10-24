"""PlatformIO pre-script: web assets -> build/web_assets.h (bridge LLD R2.3 step 5).

- web/basic_drive.html (served at /), web/manifest.webmanifest: gzip -9, served with
  Content-Encoding: gzip. Budget: the page must stay <= 25 KB gzipped (LLD R2.12).
- /icon-192.png: drawn here (no binary file in git), stored as PNG (already compressed).
- /help: web/help.html with <!--CQ:PROTOCOL--> replaced by the protogen output
  src/generated/help_protocol.html and <!--CQ:LOGO--> by web/help_logo.html (Task 9);
  skipped while web/help.html does not exist.
- /logo (Plan N4, Tank Logo LLD §21): web/logo.html with <!--CQ:LOGO_JS--> replaced by
  web/logo.js and <!--CQ:WORDS--> by web/generated/words.json. Budget 26 KB gzipped (bridge LLD R25).
- Every page: its icons from web/icons.json (scripts/icons.py, bridge LLD R25) - <!--icon:NAME--> markers and one
  /*CQ:ICONS ...*/ table; an unknown icon fails the build.
Also runnable alone: python3 scripts/pack_web.py [project_dir]
"""

from __future__ import annotations

import gzip
import pathlib
import struct
import sys
import zlib

# PlatformIO runs this file through SCons, which may not set __file__; its working directory is the project.
_HERE = pathlib.Path(__file__).resolve().parent if "__file__" in globals() else pathlib.Path.cwd() / "scripts"
sys.path.insert(0, str(_HERE))
import icons

PAGE_BUDGET = 25 * 1024
HELP_BUDGET = 16 * 1024
LOGO_BUDGET = 26 * 1024  # Plan N4 (Tank Logo LLD §21.9): /logo, gzipped, words included; R25: 26 KB with icons
SETTINGS_BUDGET = 36 * 1024  # Plan M: /settings, gzipped; Plan O (LLD R13.1): 36 KB for four tabs


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
    code = (web / "logo.js").read_text(encoding="utf-8")
    words = (web / "generated" / "words.json").read_text(encoding="utf-8").strip()
    words = words.replace("</", "<\\/")  # never closes the <script> it sits in
    return text.replace("<!--CQ:LOGO_JS-->", code).replace("<!--CQ:WORDS-->", words).encode()


def assemble_page(project: pathlib.Path, uri: str) -> bytes:
    """A page as served (before gzip), its icons put in (bridge LLD R25). /help may be missing: b""."""
    web = project / "web"
    if uri == "/":
        raw = (web / "basic_drive.html").read_bytes()
    elif uri == "/settings":
        raw = (web / "settings.html").read_bytes()
    elif uri == "/logo":
        raw = assemble_logo(project)
    else:
        raw = assemble_help(project) or b""
    table = icons.load(web / "icons.json")
    return icons.expand(raw.decode("utf-8"), table, uri).encode("utf-8")


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
