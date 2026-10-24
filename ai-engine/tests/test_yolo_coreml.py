"""The CoreML YOLO adapter (AI engine LLD §15.1): the letterbox and the box mapping are pure and tested here;
the model itself runs only when its file exists (made by tools/measure/export_yolo.py)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

from conqueror_ai.adapters import yolo_coreml as yc

MODEL = Path(__file__).resolve().parents[1] / "models" / "yolo26n-320.mlpackage"


def test_a_qvga_picture_is_letterboxed_into_the_middle() -> None:
    rgb = np.full((240, 320, 3), 200, dtype=np.uint8)
    img, (top, left, scale) = yc.letterbox(rgb, 320)
    assert img.shape == (320, 320, 3) and (top, left, scale) == (40, 0, 1.0)
    assert (img[:40] == 114).all() and (img[280:] == 114).all() and (img[40:280] == 200).all()


def test_the_model_rows_become_detections_in_the_picture() -> None:
    rows = np.array(
        [
            [10.0, 60.0, 50.0, 200.0, 0.9, 0.0],  # a person
            [100.0, 30.0, 140.0, 330.0, 0.6, 32.0],  # a ball, its bottom in the grey bar below
            [0.0, 0.0, 10.0, 10.0, 0.2, 0.0],  # too unsure
        ]
    )
    names = {0: "person", 32: "sports ball"}
    dets = yc.to_detections(rows, (40, 0, 1.0), (320, 240), names, 0.35)
    assert [d.cls for d in dets] == ["person", "sports ball"]
    assert dets[0].box == (10.0, 20.0, 50.0, 160.0)
    assert dets[1].box == (100.0, 0.0, 140.0, 240.0)  # moved back by 40 and clamped to the picture


def test_no_model_file_is_a_plain_refusal(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match="export_yolo"):
        yc.YoloCoreml(tmp_path / "missing.mlpackage")


@pytest.mark.skipif(not MODEL.exists(), reason="no model file (tools/measure/export_yolo.py makes it)")
def test_the_model_runs_on_a_picture() -> None:
    det = yc.YoloCoreml(MODEL)
    rgb = np.full((240, 320, 3), 128, dtype=np.uint8)
    out = det.detect(rgb)
    assert isinstance(out, list)
    for d in out:
        x1, y1, x2, y2 = d.box
        assert 0 <= x1 <= x2 <= 320 and 0 <= y1 <= y2 <= 240
