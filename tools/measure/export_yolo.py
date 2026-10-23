"""Make the AI engine's detector model (AI engine LLD §15.1): YOLO26n exported to CoreML at 320 px, end-to-end
(no NMS), half precision - the same export phase 0 measured (design-notes §109) - copied to ai-engine/models/.
It needs this environment's Ultralytics and torch 2.7; the engine itself needs only the CoreML runtime.

    cd tools/measure && uv run python export_yolo.py
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

from ne_budget import yolo_coreml  # exports once into tools/measure/models/, then reuses it

HERE = Path(__file__).resolve().parent
ENGINE_MODELS = HERE.parents[1] / "ai-engine" / "models"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--yolo", default="yolo26n")
    ap.add_argument("--imgsz", type=int, default=320)
    a = ap.parse_args()
    yolo_coreml(a.yolo, a.imgsz)
    src = HERE / "models" / f"{a.yolo}-{a.imgsz}.mlpackage"
    dst = ENGINE_MODELS / src.name
    ENGINE_MODELS.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    print(f"detector model: {dst}")


if __name__ == "__main__":
    main()
