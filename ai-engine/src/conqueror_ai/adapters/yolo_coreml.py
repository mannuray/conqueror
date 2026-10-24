"""The detector (AI engine LLD §15.1): YOLO26n exported to CoreML, on the Neural Engine.

The model is made once by tools/measure/export_yolo.py (whose environment has Ultralytics and torch) into
ai-engine/models/ (git-ignored: about 5 MB, AGPL). Here only Apple's CoreML runtime is needed. The export is
end-to-end: rows of x1, y1, x2, y2, score, class in the model's square input - no NMS to do."""

from __future__ import annotations

import ast
from pathlib import Path
from typing import Any

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.types import Detection

CONF = 0.35
GREY = 114  # the letterbox's padding, as Ultralytics pads


def letterbox(rgb: NDArray[np.uint8], size: int) -> tuple[NDArray[np.uint8], tuple[int, int, float]]:
    """The picture scaled to fit a size x size square and centred on grey; (top, left, scale) to undo it."""
    import cv2

    h, w = rgb.shape[:2]
    scale = min(size / w, size / h)
    nw, nh = round(w * scale), round(h * scale)
    img = rgb if (nw, nh) == (w, h) else cv2.resize(rgb, (nw, nh), interpolation=cv2.INTER_LINEAR)
    out = np.full((size, size, 3), GREY, dtype=np.uint8)
    top, left = (size - nh) // 2, (size - nw) // 2
    out[top : top + nh, left : left + nw] = img
    return out, (top, left, float(scale))


def to_detections(
    rows: NDArray[np.float64],
    pad: tuple[int, int, float],
    size: tuple[int, int],
    names: dict[int, str],
    conf: float,
) -> list[Detection]:
    """Model rows -> detections in the picture's pixels (letterbox undone, clamped to the picture)."""
    top, left, scale = pad
    w, h = size
    out: list[Detection] = []
    for x1, y1, x2, y2, score, cls in rows.tolist():
        if score < conf:
            continue
        name = names.get(int(cls))
        if name is None:
            continue
        bx = (
            min(max((x1 - left) / scale, 0.0), float(w)),
            min(max((y1 - top) / scale, 0.0), float(h)),
            min(max((x2 - left) / scale, 0.0), float(w)),
            min(max((y2 - top) / scale, 0.0), float(h)),
        )
        out.append(Detection(name, float(score), bx))
    return out


class YoloCoreml:
    def __init__(self, path: Path, conf: float = CONF) -> None:
        if not path.exists():
            raise FileNotFoundError(f"no detector model at {path} - run tools/measure/export_yolo.py")
        import coremltools as ct  # pyright: ignore[reportMissingTypeStubs]

        self._ct: Any = ct
        self._model: Any = ct.models.MLModel(str(path), compute_units=ct.ComputeUnit.ALL)
        spec: Any = self._model.get_spec()
        meta: dict[str, str] = dict(spec.description.metadata.userDefined)
        self._names: dict[int, str] = ast.literal_eval(meta["names"])
        self._size = int(ast.literal_eval(meta["imgsz"])[0])
        self._input = str(spec.description.input[0].name)
        self._output = str(spec.description.output[0].name)
        self._conf = conf

    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None = None) -> list[Detection]:
        from PIL import Image

        img, pad = letterbox(rgb, self._size)
        res: dict[str, Any] = self._model.predict({self._input: Image.fromarray(img)})
        rows = np.asarray(res[self._output], dtype=np.float64).reshape(-1, 6)
        return to_detections(rows, pad, (rgb.shape[1], rgb.shape[0]), self._names, self._conf)
