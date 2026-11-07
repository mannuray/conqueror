"""YOLO-World, asked by name (AI engine LLD §16.1 item 1): the words baked into a CoreML model, on the
Neural Engine.

The model is made by tools/measure/export_world.py into ai-engine/models/ (git-ignored, ~24 MB, GPL-3.0).
Its output
is raw: (1, 4 + words, candidates) - centre x, centre y, width, height in the 640 square, then one score per
word.
Here: a word's score is its best wording (design-notes §187), each word's own threshold, the overlap removal
(per word, then one name a thing), the letterbox undone. Measured 5.9 ms at 640."""

from __future__ import annotations

import ast
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters.yolo_coreml import letterbox
from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection
from conqueror_ai.domain.vocabulary import Vocabulary

NMS_IOU = 0.7  # as Ultralytics: overlapping chairs stay apart (checked against it, Plan Y)


PERSON = "person"  # always reported on its own score (design-notes §203)


def _iou(p: NDArray[np.float64], q: NDArray[np.float64]) -> float:
    ix = max(0.0, min(p[2], q[2]) - max(p[0], q[0]))
    iy = max(0.0, min(p[3], q[3]) - max(p[1], q[1]))
    inter = ix * iy
    a = (p[2] - p[0]) * (p[3] - p[1])
    b = (q[2] - q[0]) * (q[3] - q[1])
    return float(inter / (a + b - inter + 1e-9))


def _nms(boxes: NDArray[np.float64], scores: NDArray[np.float64]) -> list[int]:
    """Greedy non-maximum suppression: the indices kept, best first."""
    order = list(np.argsort(-scores))
    keep: list[int] = []
    while order:
        i = int(order.pop(0))
        keep.append(i)
        order = [int(j) for j in order if _iou(boxes[i], boxes[j]) < NMS_IOU]
    return keep


def to_detections(
    raw: NDArray[np.float64],
    pad: tuple[int, int, float],
    size: tuple[int, int],
    vocab: Vocabulary,
    maybe_score: float | None = None,
) -> list[Detection]:
    """The model's raw output (4 + wordings, candidates) -> detections in the picture's pixels. A word scores
    its best wording (design-notes §187). Each candidate is only its best word (as Ultralytics: one label a
    box), kept if that word's own threshold is met - or, with maybe_score, from that score up, marked unsure
    (a maybe, design-notes §179). Last, one thing, one name: of boxes of different words over nearly the same
    place (IoU >= NMS_IOU) only the stronger stays (§187). Person is the exception (design-notes §203): it is
    reported on its own score whatever word wins the box, and no other word's box removes it - so no word,
    taught or measured, can hide a person from the person guard."""
    top, left, scale = pad
    w, h = size
    a = raw.reshape(raw.shape[-2], raw.shape[-1])
    cx, cy, bw, bh = a[0], a[1], a[2], a[3]
    cols = a[4:]
    scores = np.stack([cols[[c == word for c in vocab.name_of]].max(axis=0) for word in vocab.names])
    best = scores.argmax(axis=0)
    best_score = scores.max(axis=0)
    out: list[Detection] = []
    for k, word in enumerate(vocab.names):
        bar = vocab.min_score(word) if maybe_score is None else min(maybe_score, vocab.min_score(word))
        own = scores[k]
        sel = own >= bar if word == PERSON else (best == k) & (best_score >= bar)
        if not sel.any():
            continue
        boxes = np.stack(
            [cx[sel] - bw[sel] / 2, cy[sel] - bh[sel] / 2, cx[sel] + bw[sel] / 2, cy[sel] + bh[sel] / 2],
            axis=1,
        )
        sc = own[sel].astype(np.float64)
        for i in _nms(boxes.astype(np.float64), sc):
            x1, y1, x2, y2 = boxes[i]
            box = (  # plain floats (review M5: np.float64 showed in prints and recordings)
                float(min(max((x1 - left) / scale, 0.0), float(w))),
                float(min(max((y1 - top) / scale, 0.0), float(h))),
                float(min(max((x2 - left) / scale, 0.0), float(w))),
                float(min(max((y2 - top) / scale, 0.0), float(h))),
            )
            if box[2] > box[0] and box[3] > box[1]:  # a box wholly in the grey bar is nothing
                out.append(Detection(word, float(sc[i]), box, bool(sc[i] >= vocab.min_score(word))))
    return _one_name(out)


def _one_name(dets: list[Detection]) -> list[Detection]:
    """One thing, one name (design-notes §187): a box over nearly the same place as a stronger one of another
    word is dropped; the rest keep their order. Sure boxes first: an unsure box over a kept sure one is
    dropped, whatever its score (review I4: an unsure "toy" removed a sure "chair"). A person box is never
    dropped by another word (design-notes §203) - the person guard needs it; a person still drops others."""
    keep: set[int] = set()
    for sure in (True, False):
        idx = [i for i, d in enumerate(dets) if d.sure == sure]
        idx = [
            i for i in idx if all(_iou(np.array(dets[i].box), np.array(dets[k].box)) < NMS_IOU for k in keep)
        ]
        boxes = np.array([dets[i].box for i in idx], dtype=np.float64).reshape(-1, 4)
        keep |= {idx[j] for j in _nms(boxes, np.array([dets[i].score for i in idx], dtype=np.float64))}
    people = {i for i, d in enumerate(dets) if d.cls == PERSON and not _same_word_stronger(dets, i)}
    return [d for i, d in enumerate(dets) if i in keep or i in people]


def _same_word_stronger(dets: list[Detection], i: int) -> bool:
    """A stronger person over nearly the same place: this one is the same person, already kept."""
    a = np.array(dets[i].box)
    return any(
        k != i and d.cls == PERSON and d.score > dets[i].score and _iou(a, np.array(d.box)) >= NMS_IOU
        for k, d in enumerate(dets)
    )


class YoloWorld:
    """The `Detector` port (§15.1, §16): YOLO-World on CoreML; the picture's camera is not needed."""

    def __init__(self, path: Path, vocab: Vocabulary, maybe_score: float | None = None) -> None:
        if not path.exists():
            raise FileNotFoundError(f"no detector model at {path} - run tools/measure/export_world.py")
        import coremltools as ct

        self._model: Any = ct.models.MLModel(str(path), compute_units=ct.ComputeUnit.ALL)
        spec: Any = self._model.get_spec()
        meta = dict(spec.description.metadata.userDefined)
        if "names" not in meta or "imgsz" not in meta:
            raise ValueError(
                f"{path} is not a YOLO-World export (no word list) - run tools/measure/export_world.py"
            )
        names: dict[int, str] = ast.literal_eval(meta["names"])
        vocab.check_model([names[i] for i in sorted(names)])
        self._vocab = vocab
        self._maybe = maybe_score
        self._size = int(ast.literal_eval(meta["imgsz"])[0])
        self._input = spec.description.input[0].name
        self._output = spec.description.output[0].name  # by name: a later export may add outputs

    def detect(self, rgb: NDArray[np.uint8], cam: CameraModel | None) -> list[Detection]:
        from PIL import Image

        img, pad = letterbox(rgb, self._size)
        out: dict[str, Any] = self._model.predict({self._input: Image.fromarray(img)})
        raw = np.asarray(out[self._output], dtype=np.float64)
        return to_detections(raw, pad, (rgb.shape[1], rgb.shape[0]), self._vocab, self._maybe)
