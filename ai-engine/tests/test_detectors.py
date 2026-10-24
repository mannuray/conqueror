"""What spots things (AI engine LLD §16): the words, the colour ball finder, the horizon and the joined
detectors.
Drawn pictures only - no model, no tank."""

from __future__ import annotations

from typing import cast

import cv2
import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.domain import balls as bl
from conqueror_ai.domain.detectors import MultiDetector, iou
from conqueror_ai.domain.types import Detection
from conqueror_ai.domain.vocabulary import Vocabulary
from tests.test_camera import at_start, calib

GREEN = bl.BallColour("green", 45, 90, 65, 200, 6.0)  # design-notes §152
RED = bl.BallColour("red", 0, 8, 90, 230, 6.0)


def floor_picture(w: int = 320, h: int = 240) -> NDArray[np.uint8]:
    """A pale, slightly green-tinted floor (the 'sunny' white balance): hue ~55, saturation ~30, value
    ~240."""
    hsv = np.zeros((h, w, 3), np.uint8)
    hsv[:] = (55, 30, 240)
    return cast("NDArray[np.uint8]", cv2.cvtColor(hsv, cv2.COLOR_HSV2RGB))


def disc(
    rgb: NDArray[np.uint8], centre: tuple[int, int], r: int, hsv: tuple[int, int, int]
) -> NDArray[np.uint8]:
    colour = cv2.cvtColor(np.array([[hsv]], np.uint8), cv2.COLOR_HSV2RGB)[0, 0]
    out = rgb.copy()
    cv2.circle(out, centre, r, tuple(int(c) for c in colour), -1)
    return out


# ---- the colour ball finder
def test_a_green_ball_on_the_pale_floor_is_found_with_its_box() -> None:
    pic = disc(floor_picture(), (170, 160), 12, (64, 90, 170))
    dets = bl.find_balls(pic, [GREEN], horizon=120)
    assert len(dets) == 1 and dets[0].cls == "ball:green" and dets[0].score > 0.9
    x1, y1, x2, y2 = dets[0].box
    assert abs((x1 + x2) / 2 - 170) <= 1 and abs((y1 + y2) / 2 - 160) <= 1 and abs((x2 - x1) - 24) <= 2


def test_a_ball_above_the_horizon_is_ignored() -> None:
    pic = disc(floor_picture(), (170, 60), 12, (64, 90, 170))
    assert bl.find_balls(pic, [GREEN], horizon=120) == []
    assert len(bl.find_balls(pic, [GREEN], horizon=None)) == 1  # no horizon (tilted far down): all of it


def test_a_green_square_is_not_a_ball() -> None:
    pic = floor_picture()
    colour = cv2.cvtColor(np.array([[(64, 90, 170)]], np.uint8), cv2.COLOR_HSV2RGB)[0, 0]
    cv2.rectangle(pic, (150, 140), (190, 200), tuple(int(c) for c in colour), -1)  # 40 x 60: roundness ~0.5
    assert bl.find_balls(pic, [GREEN], horizon=120) == []


def test_the_tinted_floor_alone_gives_nothing() -> None:
    assert bl.find_balls(floor_picture(), [GREEN], horizon=120) == []


def test_two_balls_of_two_colours_give_one_each() -> None:
    pic = disc(disc(floor_picture(), (100, 170), 10, (64, 90, 170)), (220, 170), 10, (3, 200, 200))
    found = sorted(d.cls for d in bl.find_balls(pic, [GREEN, RED], horizon=120))
    assert found == ["ball:green", "ball:red"]


def test_a_tiny_speck_is_not_a_ball() -> None:
    pic = disc(floor_picture(), (170, 160), 3, (64, 90, 170))
    assert bl.find_balls(pic, [GREEN], horizon=120) == []


# ---- the ball's colour from a calibration picture
def test_the_colour_range_holds_the_sample_with_its_margins() -> None:
    pic = disc(floor_picture(), (160, 170), 20, (64, 90, 170))
    pix = bl.most_coloured_blob(pic, horizon=120)
    assert pix is not None
    c = bl.colour_from_samples("green", pix, 6.0)
    assert c.h_lo <= 59 and c.h_hi >= 69 and c.s_min <= 80 and c.v_max >= 185
    assert len(bl.find_balls(pic, [c], horizon=120)) == 1  # the range finds the ball it came from
    assert bl.find_balls(floor_picture(), [c], horizon=120) == []  # and not the floor


def test_no_coloured_blob_gives_no_sample() -> None:
    assert bl.most_coloured_blob(floor_picture(), horizon=120) is None


def test_a_red_ball_whose_hue_wraps_gets_a_wrapped_range() -> None:
    """Red sits at both ends of OpenCV's hue (0 and 179): its range wraps, h_lo > h_hi (final review I4)."""
    hues = np.array([176, 177, 178, 179, 0, 1, 2, 3, 4] * 20, np.uint8)
    pix = np.stack([hues, np.full_like(hues, 160), np.full_like(hues, 170)], axis=1)
    c = bl.colour_from_samples("red", pix, 6.0)
    assert c.h_lo > c.h_hi and 165 <= c.h_lo <= 176 and 4 <= c.h_hi <= 15
    red = disc(disc(floor_picture(), (100, 170), 12, (178, 160, 170)), (220, 170), 12, (2, 160, 170))
    assert len(bl.find_balls(red, [c], horizon=120)) == 1  # both halves of red are red (the larger disc)
    green = disc(floor_picture(), (170, 160), 12, (64, 160, 170))
    assert bl.find_balls(green, [c], horizon=120) == []  # and green is not red


def test_a_spread_of_many_hues_is_not_one_colour() -> None:
    hues = np.arange(0, 180, dtype=np.uint8)
    pix = np.stack([hues, np.full_like(hues, 160), np.full_like(hues, 170)], axis=1)
    with pytest.raises(ValueError, match="not one colour"):
        bl.colour_from_samples("rainbow", pix, 6.0)


def test_a_ball_colour_out_of_range_is_refused() -> None:
    with pytest.raises(ValueError, match="hue"):
        bl.BallColour("x", 0, 200, 60, 200)
    with pytest.raises(ValueError, match="saturation"):
        bl.BallColour("x", 40, 90, 300, 200)
    with pytest.raises(ValueError, match="r_min_px"):
        bl.BallColour("x", 40, 90, 60, 200, 0.0)


def test_the_calibration_sample_is_the_round_blob_nearest_the_middle() -> None:
    """The ball is put in the middle: a larger coloured round thing at the side is not learned (review m7)."""
    pic = disc(disc(floor_picture(), (40, 200), 25, (120, 160, 170)), (160, 180), 12, (64, 160, 170))
    pix = bl.most_coloured_blob(pic, horizon=120)
    assert pix is not None and abs(float(np.median(pix[:, 0])) - 64) <= 3


# ---- the horizon row
def test_the_horizon_is_the_middle_row_with_the_camera_level() -> None:
    assert bl.horizon_row(at_start(calib())) == 120


def test_tilting_down_moves_the_horizon_up_and_far_down_takes_it_off() -> None:
    row = bl.horizon_row(at_start(calib(), tilt=-15.0))
    assert row is not None and row < 120
    assert bl.horizon_row(at_start(calib(), tilt=-60.0)) is None


def test_tilted_up_past_the_floor_nothing_in_view_is_floor() -> None:
    """Looking up (at a person), the horizon falls below the picture: no row is floor (final review m1)."""
    assert bl.horizon_row(at_start(calib(), tilt=40.0)) == 240
    pic = disc(floor_picture(), (170, 200), 12, (64, 90, 170))
    assert bl.find_balls(pic, [GREEN], horizon=240) == []


# ---- the words
def test_the_words_give_floor_classes_and_thresholds() -> None:
    v = Vocabulary.from_dict(
        {"words": [{"word": "chair"}, {"word": "door", "floor": False}, {"word": "toy", "min_score": 0.4}]}
    )
    assert v.names == ["chair", "door", "toy"]
    assert v.floor_classes == frozenset({"chair", "toy"})
    assert v.min_score("chair") == 0.25 and v.min_score("toy") == 0.4


def test_a_word_twice_or_no_words_is_refused() -> None:
    with pytest.raises(ValueError, match="chair more than once"):
        Vocabulary.from_dict({"words": [{"word": "chair"}, {"word": "chair"}]})
    with pytest.raises(ValueError, match="no words"):
        Vocabulary.from_dict({"words": []})


def test_a_model_made_for_other_words_is_refused() -> None:
    v = Vocabulary.from_dict({"words": [{"word": "chair"}, {"word": "person"}]})
    v.check_model(["chair", "person"])
    with pytest.raises(ValueError, match="re-export"):
        v.check_model(["person", "chair"])


# ---- the joined detectors
def test_any_word_box_on_a_colour_ball_is_that_ball_and_others_are_kept() -> None:
    """On the tank YOLO-World named the green ball "toy" (Plan Y draft): whatever its word, a box on a
    colour ball
    (IoU >= 0.5) is the ball - one ball, one thing."""
    colour = Detection("ball:green", 0.9, (100.0, 100.0, 124.0, 124.0))
    word_ball = Detection("ball", 0.3, (101.0, 99.0, 125.0, 125.0))
    toy = Detection("toy", 0.4, (99.0, 100.0, 123.0, 126.0))
    far_ball = Detection("ball", 0.3, (10.0, 10.0, 30.0, 30.0))
    chair = Detection(
        "chair", 0.8, (60.0, 40.0, 130.0, 160.0)
    )  # overlaps the ball a little (IoU < 0.5): kept
    m = MultiDetector([lambda rgb, h: [word_ball, toy, far_ball, chair], lambda rgb, h: [colour]])
    out = m.detect(np.zeros((240, 320, 3), np.uint8), 120)
    assert out == [far_ball, chair, colour]


def test_each_detector_gets_the_horizon() -> None:
    seen: list[int | None] = []
    m = MultiDetector([lambda rgb, h: seen.append(h) or [], lambda rgb, h: seen.append(h) or []])
    m.detect(np.zeros((4, 4, 3), np.uint8), 77)
    assert seen == [77, 77]


def test_iou() -> None:
    assert iou((0.0, 0.0, 10.0, 10.0), (0.0, 0.0, 10.0, 10.0)) == 1.0
    assert iou((0.0, 0.0, 10.0, 10.0), (20.0, 20.0, 30.0, 30.0)) == 0.0
    assert abs(iou((0.0, 0.0, 10.0, 10.0), (5.0, 0.0, 15.0, 10.0)) - 1 / 3) < 1e-9
