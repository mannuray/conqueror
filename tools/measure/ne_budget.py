"""Phase 0: how fast YOLO and Depth Anything run on this Mac, alone and together (Spec 7 §9, LLD §11).

No tank needed. Exports YOLO to CoreML (once), fetches Apple's CoreML Depth Anything V2 Small (once),
then times each model on the same picture, and both one after the other (as the engine's single worker).

    uv run python ne_budget.py [--yolo yolo26n] [--imgsz 320] [--seconds 10]
"""

from __future__ import annotations

import argparse
import statistics
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

import coremltools as ct  # type: ignore[import-untyped]
from huggingface_hub import snapshot_download  # type: ignore[import-untyped]
from PIL import Image
from ultralytics import YOLO  # type: ignore[import-untyped]
from ultralytics.utils import ASSETS  # type: ignore[import-untyped]

MODELS = Path(__file__).parent / "models"


def yolo_coreml(name: str, imgsz: int) -> Any:
    """The CoreML export of a YOLO model at this input size (exported once, then reused)."""
    target = MODELS / f"{name}-{imgsz}.mlpackage"
    if not target.exists():
        MODELS.mkdir(exist_ok=True)
        out = YOLO(f"{name}.pt").export(format="coreml", imgsz=imgsz, nms=False, half=True)
        Path(out).rename(target)
    return YOLO(str(target), task="detect")


def depth_coreml() -> Any:
    folder = snapshot_download(
        "apple/coreml-depth-anything-v2-small",
        local_dir=MODELS / "da2s",
        allow_patterns=["*F16.mlpackage/*"],
    )
    return ct.models.MLModel(
        str(Path(folder) / "DepthAnythingV2SmallF16.mlpackage"), compute_units=ct.ComputeUnit.ALL
    )


def timed(fn: Callable[[], object], seconds: float) -> list[float]:
    """Milliseconds per call, after 5 warm-up calls, for about `seconds`."""
    for _ in range(5):
        fn()
    out: list[float] = []
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        t0 = time.perf_counter()
        fn()
        out.append((time.perf_counter() - t0) * 1000)
    return out


def summary(label: str, ms: list[float]) -> str:
    med = statistics.median(ms)
    p90 = sorted(ms)[int(len(ms) * 0.9) - 1]
    return f"{label:<28} median {med:6.1f} ms  p90 {p90:6.1f} ms  -> {1000 / med:5.1f} per second"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--yolo", default="yolo26n")
    ap.add_argument("--imgsz", type=int, default=320)
    ap.add_argument("--seconds", type=float, default=10)
    a = ap.parse_args()
    picture = Image.open(ASSETS / "bus.jpg").convert("RGB")
    yolo = yolo_coreml(a.yolo, a.imgsz)
    depth = depth_coreml()
    small = picture.resize((518, 392))

    def run_yolo() -> object:
        return yolo.predict(picture, imgsz=a.imgsz, verbose=False)

    def run_depth() -> object:
        return depth.predict({"image": small})

    def run_both() -> object:
        run_yolo()
        return run_depth()

    print(summary(f"{a.yolo} @ {a.imgsz}", timed(run_yolo, a.seconds)))
    print(summary("Depth Anything V2 Small", timed(run_depth, a.seconds)))
    print(summary("both, one after the other", timed(run_both, a.seconds)))


if __name__ == "__main__":
    main()
