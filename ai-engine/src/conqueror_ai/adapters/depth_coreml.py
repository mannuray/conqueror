"""The depth model (AI engine LLD §18.4): Depth Anything V2 Small, Apple's CoreML build, on the Neural Engine.

The model file is the one downloaded for phase 0 (tools/measure/models/da2s, design-notes §109), copied to
ai-engine/models/ (git-ignored). It takes a 518 x 392 RGB picture and gives relative inverse depth (along the
camera's axis) at the same size; here it is resized back to the picture's size."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import cv2
import numpy as np
from numpy.typing import NDArray
from PIL import Image

SIZE = (518, 392)  # the model's input and output (width, height)


class DepthCoreml:
    def __init__(self, path: Path) -> None:
        import coremltools as ct

        self.model: Any = ct.models.MLModel(str(path), compute_units=ct.ComputeUnit.ALL)

    def infer(self, rgb: NDArray[np.uint8]) -> NDArray[np.float32]:
        """Relative inverse depth, one value per pixel of the picture (larger = nearer)."""
        h, w = rgb.shape[:2]
        img = Image.fromarray(rgb).resize(SIZE, Image.Resampling.BILINEAR)
        out: Any = self.model.predict({"image": img})["depth"]
        inv = np.asarray(out, dtype=np.float32)
        if inv.ndim == 3:  # the model gives a 2-D float image; a (1, h, w) array is taken too, to be safe
            inv = inv[0]
        return cv2.resize(inv, (w, h), interpolation=cv2.INTER_LINEAR).astype(np.float32)
