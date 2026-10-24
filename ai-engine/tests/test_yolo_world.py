"""The YOLO-World adapter (AI engine LLD §16.1 item 1): the raw output -> detections is pure and tested
here; the
model itself runs only when its file exists (made by tools/measure/export_world.py)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.adapters import yolo_world as yw
from conqueror_ai.domain.vocabulary import Vocabulary

MODEL = Path(__file__).resolve().parents[1] / "models" / "yoloworld-s-640.mlpackage"
VOCAB = Vocabulary.from_dict({"words": [{"word": "person"}, {"word": "chair", "min_score": 0.5}]})


def raw(*cands: tuple[float, float, float, float, float, float]) -> NDArray[np.float64]:
    """Candidates as (cx, cy, w, h, person score, chair score) -> the model's (1, 6, n) output."""
    return np.array(cands, dtype=np.float64).T[None]


def test_each_word_keeps_its_own_threshold_and_the_letterbox_is_undone() -> None:
    out = raw(
        (100.0, 200.0, 40.0, 100.0, 0.30, 0.0),  # a person at 0.30 (>= 0.25)
        (300.0, 300.0, 60.0, 60.0, 0.0, 0.40),  # a chair at 0.40 (< its 0.5)
        (400.0, 300.0, 60.0, 60.0, 0.0, 0.70),  # a chair at 0.70
    )
    dets = yw.to_detections(out, (80, 0, 1.0), (640, 480), VOCAB)
    assert [(d.cls, round(d.score, 2)) for d in dets] == [("person", 0.3), ("chair", 0.7)]
    assert dets[0].box == (80.0, 70.0, 120.0, 170.0)  # y moved back by the 80-row grey bar


def test_overlapping_boxes_of_one_word_become_one_and_other_words_stay() -> None:
    out = raw(
        (200.0, 200.0, 100.0, 100.0, 0.9, 0.0),
        (205.0, 200.0, 100.0, 100.0, 0.8, 0.0),  # the same person (IoU ~0.9): dropped
        (205.0, 200.0, 100.0, 100.0, 0.0, 0.6),  # a chair there: another word, kept
        (500.0, 200.0, 100.0, 100.0, 0.7, 0.0),  # another person
    )
    dets = yw.to_detections(out, (0, 0, 1.0), (640, 640), VOCAB)
    assert sorted((d.cls, round(d.score, 1)) for d in dets) == [
        ("chair", 0.6),
        ("person", 0.7),
        ("person", 0.9),
    ]


def test_a_box_is_only_its_best_word() -> None:
    """As Ultralytics (one label a box): a cabinet scoring bed 0.40 and sofa 0.31 is one bed, not a bed and a
    sofa (final review I1); a best word under its own threshold is dropped, not replaced by the next word."""
    v = Vocabulary.from_dict({"words": [{"word": "person"}, {"word": "chair", "min_score": 0.5}]})
    dets = yw.to_detections(
        raw(
            (200.0, 200.0, 100.0, 100.0, 0.40, 0.31),  # person best: one person (chair is under 0.5 anyway)
            (400.0, 200.0, 100.0, 100.0, 0.30, 0.60),  # chair best: one chair, the person 0.30 is not listed
            (500.0, 400.0, 100.0, 100.0, 0.30, 0.45),  # chair best but under its 0.5: nothing
        ),
        (0, 0, 1.0),
        (640, 640),
        v,
    )
    assert sorted((d.cls, round(d.score, 2)) for d in dets) == [("chair", 0.6), ("person", 0.4)]


def test_a_box_wholly_in_the_grey_bar_is_dropped() -> None:
    dets = yw.to_detections(raw((100.0, 40.0, 20.0, 20.0, 0.9, 0.0)), (80, 0, 1.0), (640, 480), VOCAB)
    assert dets == []


def test_boxes_are_clamped_to_the_picture() -> None:
    dets = yw.to_detections(raw((10.0, 90.0, 60.0, 40.0, 0.9, 0.0)), (80, 0, 1.0), (640, 480), VOCAB)
    assert dets[0].box == (0.0, 0.0, 40.0, 30.0)


def test_no_model_file_is_a_plain_refusal(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match="export_world"):
        yw.YoloWorld(tmp_path / "missing.mlpackage", VOCAB)


@pytest.mark.skipif(not MODEL.exists(), reason="no model file (tools/measure/export_world.py makes it)")
def test_the_model_refuses_other_words_and_runs_with_its_own() -> None:
    with pytest.raises(ValueError, match="re-export"):
        yw.YoloWorld(MODEL, VOCAB)
    from conqueror_ai.adapters.detector_config import load_vocabulary

    vocab = load_vocabulary(Path(__file__).resolve().parents[1] / "config" / "vocabulary.yaml")
    det = yw.YoloWorld(MODEL, vocab)
    rng = np.random.default_rng(1)
    pic = rng.integers(0, 256, (480, 640, 3), dtype=np.uint8)  # noise: whatever it finds must be well formed
    out = det.detect(pic, 240)
    assert isinstance(out, list)
    for d in out:
        x1, y1, x2, y2 = d.box
        assert d.cls in vocab.names and d.score >= vocab.min_score(d.cls)
        assert 0 <= x1 < x2 <= 640 and 0 <= y1 < y2 <= 480
