"""`cq-replay`: show a recorded session in the Rerun viewer, as fast as it can (Spec 7 phase 1; later the map
pipeline replays through the same reader).

    uv run cq-replay recordings/20261021-101500
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from conqueror_ai.adapters.session import replay
from conqueror_ai.domain.telemetry import parse_td


def counts(folder: Path) -> tuple[int, int]:
    """(pictures, td samples) of a session - also a quick check that it reads back."""
    pictures = samples = 0
    for e in replay(folder):
        if e.frame is not None:
            pictures += 1
        elif e.text is not None and parse_td(json.loads(e.text)) is not None:
            samples += 1
    return pictures, samples


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path)
    a = ap.parse_args()
    from conqueror_ai.adapters.rerun_view import RerunView

    view = RerunView(spawn=True)
    for e in replay(a.folder):
        if e.frame is not None:
            view.frame(e.frame, None)
        elif e.text is not None:
            s = parse_td(json.loads(e.text))
            if s is not None:
                view.telemetry(s)
    pictures, samples = counts(a.folder)
    print(f"{pictures} pictures, {samples} telemetry samples replayed from {a.folder}")


if __name__ == "__main__":
    main()
