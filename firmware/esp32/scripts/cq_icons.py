"""The tank pages' shared icons (bridge LLD R25): Lucide bodies from web/icons.json put into each page when packing.

- `<!--icon:NAME attrs-->` becomes `<svg viewBox="0 0 24 24" attrs>BODY</svg>` (the tag the pages always used).
- `/*CQ:ICONS a b c*/` in a script becomes `var ICONS = {...}` with only those icons, and `icon(name)`.
An unknown name fails the build, naming the page. Pure: no files read here except by `load`.
"""

from __future__ import annotations

import json
import pathlib
import re

# attributes are name="value" pairs without < > in the value; anything else is left over and fails (below)
MARKER = re.compile(r'<!--icon:([a-z0-9-]*)((?:\s+[a-zA-Z][\w-]*="[^"<>]*")*)\s*-->')
TABLE = re.compile(r"/\*CQ:ICONS([^*]*)\*/")
# what a person might have meant as a marker or a table, in any spelling; left after expanding, it fails the build
LEFT_MARKER = re.compile(r"<!--\s*icon\s*:", re.IGNORECASE)
LEFT_TABLE = re.compile(r"/\*\s*CQ\s*:\s*ICONS", re.IGNORECASE)
CALL = re.compile(r"(?<![\w.$])icon\(")
LITERAL = re.compile(r'(?<![\w.$])icon\("([^"]*)"\)')
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
        'function icon(n) { if (!(n in ICONS)) console.error("icon: " + n + " is not in this page\'s icon table"); '
        "return '" + SVG_OPEN + '>\' + ICONS[n] + "</svg>"; }'
    )
    return TABLE.sub(lambda _m: code, text)


def check_calls(text: str, page: str) -> None:
    """Every literal icon("x") must be in the page's table, and icon() needs a table (before expanding)."""
    found = TABLE.findall(text)
    listed = set(found[0].split()) if found else set()
    if CALL.search(text) and not found:
        raise SystemExit(f"pack_web: {page} calls icon() but has no /*CQ:ICONS*/ table")
    for name in LITERAL.findall(text):
        if name not in listed:
            raise SystemExit(
                f'pack_web: {page} calls icon("{name}"), which is not in its /*CQ:ICONS*/ table'
            )


def check_leftovers(text: str, page: str) -> None:
    """After expanding: a marker or table written any other way is a mistake, not text."""
    for pattern, what in (
        (LEFT_MARKER, "an icon marker"),
        (LEFT_TABLE, "a CQ:ICONS table"),
    ):
        m = pattern.search(text)
        if m:
            raise SystemExit(
                f"pack_web: {page} has {what} that cannot be read: {text[m.start() : m.start() + 60]!r}"
            )


def expand(text: str, icons: dict[str, str], page: str) -> str:
    check_calls(text, page)
    out = expand_markers(expand_table(text, icons, page), icons, page)
    check_leftovers(out, page)
    return out


def lucide_body(svg: str) -> str:
    """The inside of a lucide-static icon file, compact (as in the pages): `<path d="..."/><rect .../>`."""
    inner = svg[svg.index(">", svg.index("<svg")) + 1 : svg.rindex("</svg>")]
    inner = re.sub(r"\s*/>", "/>", inner.strip())
    return re.sub(r">\s+<", "><", inner)
