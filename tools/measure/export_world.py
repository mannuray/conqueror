"""Make the AI engine's detector model (AI engine LLD §16.1 item 1): YOLO-World (yolov8s-worldv2, GPL-3.0)
with the words of ai-engine/config/vocabulary.yaml
(and their extra wordings, design-notes §187) baked in, exported to CoreML at 640 (raw output, no NMS, half
precision), copied to ai-engine/models/yoloworld-s-640.mlpackage. Run again after changing the words.
It needs this environment's Ultralytics and Ultralytics' CLIP (for the words, only here); the engine itself
needs only the CoreML runtime. Measured 5.9 ms a picture on the M1's Neural Engine (Plan Y draft).

    cd tools/measure && uv run python export_world.py
    uv run python export_world.py --extra ../../ai-engine/taught/vocabulary.yaml --out /tmp/new-model
--extra adds a file's words after the measured ones (the words taught from the /ai page, AI engine LLD
§22.4); --out writes the model and words.txt (the exact list, one per line) into that folder instead of
ai-engine/models/, for the engine to move into ai-engine/taught/.
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
    ap.add_argument("--extra", type=Path, help="more words after the measured ones (taught words)")
    ap.add_argument("--out", type=Path, help="write the model and words.txt here, not into ai-engine/models")
    a = ap.parse_args()
    rows = yaml.safe_load(a.vocabulary.read_text())["words"]
    if a.extra is not None:
        rows += yaml.safe_load(a.extra.read_text()).get("words") or []
    words = [str(p) for w in rows for p in (w["word"], *(w.get("also") or []))]  # each word, its wordings
    local = HERE / "models" / a.weights  # reused when already downloaded
    model = YOLOWorld(str(local) if local.exists() else a.weights)
    model.set_classes(words)
    out = Path(model.export(format="coreml", imgsz=a.imgsz, nms=False, half=True))
    folder = a.out if a.out is not None else ENGINE / "models"
    dst = folder / f"yoloworld-s-{a.imgsz}.mlpackage"
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        shutil.rmtree(dst)
    shutil.move(str(out), dst)
    if a.out is not None:
        (folder / "words.txt").write_text("".join(f"{w}\n" for w in words))
    print(f"detector model: {dst} ({len(words)} words: {', '.join(words)})")


if __name__ == "__main__":
    main()
