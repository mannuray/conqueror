"""Phase 0: the depth model's value at a chosen point of a saved picture (the box may sit anywhere in the
picture - the middle of the picture is often above a low box). Then fit the values against the tape.

    uv run python depth_point.py value out/box-030.jpg 320 340     # x, y on the 640x480 picture
    uv run python depth_point.py fit 0.3:0.61 0.6:0.42 1.0:0.30 2.0:0.18   # tape m : value
"""

from __future__ import annotations

import argparse

import cv2
import numpy as np
from PIL import Image

from measure_lib import fit_inverse_depth
from ne_budget import depth_coreml


def value_at(jpeg: bytes, x: float, y: float, half: int = 6) -> float:
    """Median model value in a small square around (x, y), given in the picture's own pixels."""
    bgr = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
    h, w = bgr.shape[:2]
    pic = Image.fromarray(cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)).resize((518, 392))
    depth = np.asarray(depth_coreml().predict({"image": pic})["depth"], dtype=np.float32)  # type: ignore[union-attr]
    dh, dw = depth.shape
    cx, cy = int(x * dw / w), int(y * dh / h)
    return float(np.median(depth[max(0, cy - half) : cy + half, max(0, cx - half) : cx + half]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("value")
    v.add_argument("picture")
    v.add_argument("x", type=float)
    v.add_argument("y", type=float)
    f = sub.add_parser("fit")
    f.add_argument("pairs", nargs="+", help="tape_m:value")
    a = ap.parse_args()
    if a.cmd == "value":
        with open(a.picture, "rb") as fh:
            print(f"{value_at(fh.read(), a.x, a.y):.4f}")
        return
    tape = [float(p.split(":")[0]) for p in a.pairs]
    rel = [float(p.split(":")[1]) for p in a.pairs]
    for inverse in (True, False):
        fit = fit_inverse_depth(tape, rel, inverse)
        print(f"{'1/d' if inverse else 'd'} = {fit.a:.4f} r + {fit.c:.4f}")
        for t, m, e in fit.rows:
            print(f"  tape {t:.2f} m -> model {m:.2f} m ({e:+.1f} %)")


if __name__ == "__main__":
    main()
