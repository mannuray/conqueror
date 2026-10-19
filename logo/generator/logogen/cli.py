"""logogen write | check (LLD §15)."""

from __future__ import annotations

import sys
from pathlib import Path

from logogen import emit, model

ROOT = Path(__file__).resolve().parents[3]
ESP = ROOT / "firmware" / "esp32"
OUTPUTS = {
    ESP / "src" / "logo" / "generated" / "words.h": emit.render_words_h,
    ESP / "web" / "generated" / "words.json": emit.render_words_json,
    ESP / "test" / "test_logo_conformance" / "cases.h": emit.render_cases_h,
    ESP / "test" / "test_logo_running" / "cases.h": emit.render_running_h,
}


def main() -> int:
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    spec = model.load(ROOT)
    stale: list[Path] = []
    for path, render in OUTPUTS.items():
        text = render(spec)
        if cmd == "write":
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        elif not path.exists() or path.read_text() != text:
            stale.append(path.relative_to(ROOT))
    if cmd == "write":
        print(f"logogen: wrote {len(OUTPUTS)} files")
        return 0
    for s in stale:
        print(f"logogen: stale {s} - run: cd logo && uv run logogen write")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
