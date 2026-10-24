"""Make the AI engine's detector model (AI engine LLD §16.1 item 1): YOLO-World (yolov8s-worldv2, GPL-3.0)
with the words of ai-engine/config/vocabulary.yaml baked in, exported to CoreML at 640 (raw output, no NMS,
half precision), copied to ai-engine/models/yoloworld-s-640.mlpackage. Run again after changing the words.
It needs this environment's Ultralytics and Ultralytics' CLIP (for the words, only here); the engine itself
needs only the CoreML runtime. Measured 5.9 ms a picture on the M1's Neural Engine (Plan Y draft).

    cd tools/measure && uv run python export_world.py
"""

from __future__ import annotations

import argparse
import shutil
from pathlib import Path

import yaml
from ultralytics import YOLOWorld

HERE = Path(__file__).resolve().parent
ENGINE = HERE.parents[1] / "ai-engine"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights", default="yolov8s-worldv2.pt")  # Ultralytics downloads it once (~25 MB)
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--vocabulary", type=Path, default=ENGINE / "config" / "vocabulary.yaml")
    a = ap.parse_args()
    words = [str(w["word"]) for w in yaml.safe_load(a.vocabulary.read_text())["words"]]
    local = HERE / "models" / a.weights  # reused when already downloaded
    model = YOLOWorld(str(local) if local.exists() else a.weights)
    model.set_classes(words)
    out = Path(model.export(format="coreml", imgsz=a.imgsz, nms=False, half=True))
    dst = ENGINE / "models" / f"yoloworld-s-{a.imgsz}.mlpackage"
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        shutil.rmtree(dst)
    shutil.move(str(out), dst)
    print(f"detector model: {dst} ({len(words)} words: {', '.join(words)})")


if __name__ == "__main__":
    main()
