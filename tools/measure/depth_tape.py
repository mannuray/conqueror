"""Phase 0: Depth Anything against a tape measure (and the ultrasonic), on one floor.

Put a box (at least 20 cm wide and tall) straight ahead of the tank at each tape distance; press Enter for
each. The script grabs a picture and the ultrasonic distance, takes the depth model's value in the middle
of the picture, then fits 1/d = a*r + c over all marks and prints the error of every mark. Run it once on
the wood floor and once on the rug (--floor). Nothing moves. Pictures are kept in out/ (git-ignored).

    uv run python depth_tape.py --floor wood [--marks 0.3,0.6,1.0,2.0]
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from bridge import DEFAULT_HOST, Bridge, frames
from measure_lib import fit_inverse_depth
from ne_budget import depth_coreml

OUT = Path(__file__).parent / "out"


def one_picture(host: str) -> bytes:
    for _, jpeg in frames(host):
        return jpeg  # the first whole picture
    raise RuntimeError("no picture from the stream")


def centre_value(model: object, jpeg: bytes) -> float:
    rgb = cv2.cvtColor(cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR), cv2.COLOR_BGR2RGB)
    pic = Image.fromarray(rgb).resize((518, 392))
    depth = np.asarray(model.predict({"image": pic})["depth"], dtype=np.float32)  # type: ignore[attr-defined]
    h, w = depth.shape
    return float(np.median(depth[h // 2 - 20 : h // 2 + 20, w // 2 - 20 : w // 2 + 20]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--floor", required=True)
    ap.add_argument("--marks", default="0.3,0.6,1.0,2.0")
    a = ap.parse_args()
    marks = [float(m) for m in a.marks.split(",")]
    model = depth_coreml()
    bridge = Bridge(a.host)
    bridge.debug(10)
    OUT.mkdir(exist_ok=True)
    rel: list[float] = []
    sonar: list[float | None] = []
    try:
        for m in marks:
            input(f"Box at {m:.2f} m straight ahead, then Enter ")
            time.sleep(0.5)
            jpeg = one_picture(a.host)
            (OUT / f"depth-{a.floor}-{m:.2f}.jpg").write_bytes(jpeg)
            rel.append(centre_value(model, jpeg))
            td = bridge.latest_td()
            d = td.get("dist_mm") if td else None
            sonar.append(d / 1000 if isinstance(d, int | float) else None)
    finally:
        bridge.close()
    for inverse in (True, False):  # which way round the model's output is shows in the smaller errors
        fit = fit_inverse_depth(marks, rel, inverse)
        print(f"floor {a.floor}: {'1/d' if inverse else 'd'} = {fit.a:.4f} r + {fit.c:.4f}")
        print("tape m | depth model m | error % | ultrasonic m")
        for (tape, model_m, err), s in zip(fit.rows, sonar, strict=True):
            print(f"{tape:6.2f} | {model_m:13.2f} | {err:7.1f} | {s if s is not None else 'no echo'}")


if __name__ == "__main__":
    main()
