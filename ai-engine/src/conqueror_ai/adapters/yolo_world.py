"""YOLO-World, asked by name (AI engine LLD §16.1 item 1): the words baked into a CoreML model, on the
Neural Engine.

The model is made by tools/measure/export_world.py into ai-engine/models/ (git-ignored, ~24 MB, GPL-3.0).
Its output
is raw: (1, 4 + words, candidates) - centre x, centre y, width, height in the 640 square, then one score per
word.
Here: each word's own threshold, the overlap removal (per word), the letterbox undone. Measured 5.9 ms at
640."""

from __future__ import annotations

import ast
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters.yolo_coreml import letterbox
from conqueror_ai.domain.types import Detection
from conqueror_ai.domain.vocabulary import Vocabulary

NMS_IOU = 0.7  # as Ultralytics: overlapping chairs stay apart (checked against it, Plan Y)


def _nms(boxes: NDArray[np.float64], scores: NDArray[np.float64]) -> list[int]:
    """Greedy non-maximum suppression: the indices kept, best first."""
    order = list(np.argsort(-scores))
    keep: list[int] = []
    while order:
        i = int(order.pop(0))
        keep.append(i)
        rest: list[int] = []
        for j in order:
            ix = max(0.0, min(boxes[i, 2], boxes[j, 2]) - max(boxes[i, 0], boxes[j, 0]))
            iy = max(0.0, min(boxes[i, 3], boxes[j, 3]) - max(boxes[i, 1], boxes[j, 1]))
            inter = ix * iy
            a = (boxes[i, 2] - boxes[i, 0]) * (boxes[i, 3] - boxes[i, 1])
            b = (boxes[j, 2] - boxes[j, 0]) * (boxes[j, 3] - boxes[j, 1])
            if inter / (a + b - inter + 1e-9) < NMS_IOU:
                rest.append(int(j))
        order = rest
    return keep


def to_detections(
    raw: NDArray[np.float64],
    pad: tuple[int, int, float],
    size: tuple[int, int],
    vocab: Vocabulary,
) -> list[Detection]:
    """The model's raw output (4 + words, candidates) -> detections in the picture's pixels. Each candidate is
    only its best word (as Ultralytics: one label a box), kept if that word's own threshold is met."""
    top, left, scale = pad
    w, h = size
    a = raw.reshape(raw.shape[-2], raw.shape[-1])
    cx, cy, bw, bh = a[0], a[1], a[2], a[3]
    scores = a[4:]
    best = scores.argmax(axis=0)
    best_score = scores.max(axis=0)
    out: list[Detection] = []
    for k, word in enumerate(vocab.names):
        sel = (best == k) & (best_score >= vocab.min_score(word))
        if not sel.any():
            continue
        boxes = np.stack(
            [cx[sel] - bw[sel] / 2, cy[sel] - bh[sel] / 2, cx[sel] + bw[sel] / 2, cy[sel] + bh[sel] / 2],
            axis=1,
        )
        sc = best_score[sel].astype(np.float64)
        for i in _nms(boxes.astype(np.float64), sc):
            x1, y1, x2, y2 = boxes[i]
            box = (
                min(max((x1 - left) / scale, 0.0), float(w)),
                min(max((y1 - top) / scale, 0.0), float(h)),
                min(max((x2 - left) / scale, 0.0), float(w)),
                min(max((y2 - top) / scale, 0.0), float(h)),
            )
            if box[2] > box[0] and box[3] > box[1]:  # a box wholly in the grey bar is nothing
                out.append(Detection(word, float(sc[i]), box))
    return out


class YoloWorld:
    """The `Detector` port (§15.1, §16): YOLO-World on CoreML; the horizon is not needed."""

    def __init__(self, path: Path, vocab: Vocabulary) -> None:
        if not path.exists():
            raise FileNotFoundError(f"no detector model at {path} - run tools/measure/export_world.py")
        import coremltools as ct

        self._model: Any = ct.models.MLModel(str(path), compute_units=ct.ComputeUnit.ALL)
        spec: Any = self._model.get_spec()
        meta = dict(spec.description.metadata.userDefined)
        names: dict[int, str] = ast.literal_eval(meta["names"])
        vocab.check_model([names[i] for i in sorted(names)])
        self._vocab = vocab
        self._size = int(ast.literal_eval(meta["imgsz"])[0])
        self._input = spec.description.input[0].name
        self._output = spec.description.output[0].name  # by name: a later export may add outputs

    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None) -> list[Detection]:
        from PIL import Image

        img, pad = letterbox(rgb, self._size)
        out: dict[str, Any] = self._model.predict({self._input: Image.fromarray(img)})
        raw = np.asarray(out[self._output], dtype=np.float64)
        return to_detections(raw, pad, (rgb.shape[1], rgb.shape[0]), self._vocab)
