"""Phase 0: show the live camera picture with a centre line and save a picture on Enter (mount and blind
zone). Nothing moves. Pictures go to out/ (git-ignored: they show the home). Esc or q ends.

    uv run python snap.py [--host 192.168.31.167]
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import cv2
import numpy as np

from bridge import DEFAULT_HOST, frames

OUT = Path(__file__).parent / "out"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    a = ap.parse_args()
    OUT.mkdir(exist_ok=True)
    for _, jpeg in frames(a.host):
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if img is None:  # pyright: ignore[reportUnnecessaryComparison] - None for a broken picture
            continue
        h, w = img.shape[:2]
        shown = img.copy()
        cv2.line(shown, (w // 2, 0), (w // 2, h), (0, 255, 0), 1)
        cv2.line(shown, (0, h - 1), (w, h - 1), (0, 0, 255), 2)  # the bottom row: the blind zone starts here
        cv2.imshow("tank camera (Enter = save, q = quit)", shown)
        key = cv2.waitKey(1) & 0xFF
        if key in (13, 10):
            name = OUT / f"snap-{time.strftime('%H%M%S')}.jpg"
            name.write_bytes(jpeg)
            print(f"saved {name}")
        elif key in (27, ord("q")):
            break
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
