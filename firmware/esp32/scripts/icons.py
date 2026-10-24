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
