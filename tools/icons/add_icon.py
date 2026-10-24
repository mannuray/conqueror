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
import cq_icons as icons

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


def main(argv: list[str], file: pathlib.Path = FILE) -> None:
    local = None
    if argv[:1] == ["--from"]:
        if len(argv) < 2:
            raise SystemExit(
                "add_icon: --from needs a folder (an unpacked lucide-static package)"
            )
        local, argv = pathlib.Path(argv[1]), argv[2:]
    if not argv:
        raise SystemExit("add_icon: name the icons to add, e.g. add_icon.py radar plug")
    licence = "ISC, (c) Lucide Contributors (portions (c) Cole Bemis, Feather, MIT)"
    data = (
        json.loads(file.read_text(encoding="utf-8"))
        if file.exists()
        else {"licence": licence, "icons": {}}
    )
    data["source"] = (
        f"lucide-static {VERSION}"  # every write says which Lucide the bodies come from
    )
    for name in argv:
        if name in data["icons"]:
            print(f"add_icon: replaced {name}")
        data["icons"][name] = icons.lucide_body(fetch(name, local))
    data = {
        "source": data["source"],
        "licence": data.get("licence", licence),
        "icons": dict(sorted(data["icons"].items())),
    }
    file.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")
    print(f"add_icon: {len(argv)} added, {len(data['icons'])} in {file.name}")


if __name__ == "__main__":
    main(sys.argv[1:])
