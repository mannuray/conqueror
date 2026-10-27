"""The depth model on the neural engine (AI engine LLD §18.4)."""

from __future__ import annotations

import numpy as np
import pytest

from conqueror_ai.app.objects import DEPTH_MODEL


@pytest.mark.skipif(not DEPTH_MODEL.exists(), reason="no model file (copy it from tools/measure/models/da2s)")
def test_the_model_gives_inverse_depth_at_the_pictures_size() -> None:
    from conqueror_ai.adapters.depth_coreml import DepthCoreml

    rng = np.random.default_rng(0)
    rgb = rng.integers(0, 255, (480, 640, 3), dtype=np.uint8)
    inv = DepthCoreml(DEPTH_MODEL).infer(rgb)
    assert inv.shape == (480, 640) and inv.dtype == np.float32 and np.isfinite(inv).all()
