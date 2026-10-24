# Plan Y — What spots things: YOLO-World and a colour ball finder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The tank lists the things in the room from floor level: YOLO-World asked by name, plus a colour finder for the child's balls. Both sit under the `Detector` plug of §15, so the tracker, the floor trick and `cq-objects` stay as they are. The camera works at VGA.

**Architecture:**
- **Pure (domain):**
  - `Vocabulary` (the words file);
  - `MultiDetector` (joins the detectors; any word box on a colour ball is that ball);
  - `balls.py` (the colour finder, the horizon row, a ball's colour from samples).
- **Adapters:**
  - `YoloWorld` (CoreML: raw output → each word's threshold → overlap removal → letterbox undone);
  - `detector_config` (the YAML files);
  - `calibration_store` (VGA from the QVGA file, ×2).
- **App:**
  - `cq-objects` builds both detectors and passes each picture's horizon row;
  - `cq-calibrate ball <name>`.
- `tools/measure/export_world.py` bakes the words into the model.

**Tech Stack:** Python 3.12 (uv, ruff, pyright strict), OpenCV, CoreML (`coremltools`); `tools/measure` (Ultralytics + Ultralytics' CLIP) for the export only.

**Spec:** `docs/lld/ai-engine-lld.md` §16 (approved 2026-10-24); design-notes §151–§152.

**Built from a tested draft:** branch `y-draft`.
- AI engine 167 passed, 1 skipped (the old YOLO26n model test, whose file isn't there); ruff, format and pyright clean.
- **Speed:** YOLO-World at 640: 5.9 ms a picture on the M1's Neural Engine. Both detectors at VGA: median 7.3 ms, p95 7.7 ms.
- **Real recording replayed** (ball ~1 m, office chair, cabinet): `ball:green` placed 106 cm ahead of the tank's centre, 11 cm right (97.5 cm from the lens; tape ~1 m), plus `chair` at 205 cm.
- **The colour finder on 139 real VGA pictures:** found in all 139, centre steady to ±1.8 px.
- **Our CoreML YOLO-World against Ultralytics' own** on COCO picture 139: the same person 0.83 and chairs 0.76 / 0.58 / 0.54, boxes within 1 px.

## Global Constraints
- **The `Detector` plug becomes `detect(rgb, horizon_row: int | None) -> list[Detection]`** (everything else in §15 unchanged).
- **The colour finder:**
  - HSV of a 5 × 5 blur; a mask per ball, with rows above the horizon cleared; open 3 × 3;
  - the largest contour with convex-hull roundness `hull area / (π r²)` ≥ **0.66** and `r ≥ r_min_px`;
  - score = `min(1, roundness × 1.3)`; class `ball:<name>`.
- **The words** (`config/vocabulary.yaml`): person, chair, table, bed, sofa, door (`floor: false`), bottle, shoe, box, toy. Threshold 0.25 unless set; a model made for other words is refused.
- **The green ball** (`config/balls.yaml`): `h [45, 90]`, `s_min 65`, `v_max 200`, `r_min_px 6`, measured at camera setting **B** (brightness −1, contrast +2, low light on, white balance "sunny").
- **VGA:** the calibration file stays at QVGA; `load(path, (640, 480))` scales fx and fy ×2, and `c' = (c + 0.5) × 2 − 0.5`.
- **Models are git-ignored** (`ai-engine/models/`). YOLO-World is GPL-3.0 (home project, not sold).
- **Hardware:** recordings show the home. Keep them local and delete them once the numbers are written down. Nothing drives; Task 5 Step 3 turns only the camera.

## Review Focus
1. **One ball must be one thing**, even when YOLO-World names it something else ("toy" on the tank). Pinned by `test_any_word_box_on_a_colour_ball_is_that_ball_and_others_are_kept` and the VGA replay test.
2. **The pale, green-tinted floor must never become a ball.** Its hue is the ball's; only saturation and value differ. Pinned by `test_the_tinted_floor_alone_gives_nothing`, plus the real recording on the floor (Task 5).
3. **A model exported for other words must not run silently with wrong names.** Pinned by `test_a_model_made_for_other_words_is_refused` and the real-model test.
4. **VGA distances need the scaled calibration to be right.** Pinned by `test_vga_is_the_qvga_file_at_twice_the_pixels`, and the checkerboard check on the tank (Task 5 Step 1).
5. **The horizon must come from the picture's own camera angles** (tilted down, the floor starts higher). Pinned by `test_tilting_down_moves_the_horizon_up_and_far_down_takes_it_off`.

---

### Task 1: The words, the colour ball finder and the joiner (pure)

**Files:**
- Create:
  - `ai-engine/src/conqueror_ai/domain/vocabulary.py`
  - `ai-engine/src/conqueror_ai/domain/balls.py`
  - `ai-engine/src/conqueror_ai/domain/detectors.py`
  - `ai-engine/tests/test_detectors.py`

**Interfaces:**
- Produces:
  - `Vocabulary.from_dict(d)`, `.names`, `.floor_classes`, `.min_score(word)`, `.check_model(names)`;
  - `BallColour(name, h_lo, h_hi, s_min, v_max, r_min_px)`;
  - `find_balls(rgb, balls, horizon) -> list[Detection]`;
  - `horizon_row(cam) -> int | None`;
  - `colour_from_samples(name, hsv_pixels, r_min_px)`;
  - `most_coloured_blob(rgb, horizon)`;
  - `MultiDetector([fn(rgb, horizon) -> list[Detection], ...]).detect(rgb, horizon_row)`;
  - `iou(a, b)`.

- [ ] **Step 1: Write the tests**, `ai-engine/tests/test_detectors.py`:

```python
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


# ---- the horizon row
def test_the_horizon_is_the_middle_row_with_the_camera_level() -> None:
    assert bl.horizon_row(at_start(calib())) == 120


def test_tilting_down_moves_the_horizon_up_and_far_down_takes_it_off() -> None:
    row = bl.horizon_row(at_start(calib(), tilt=-15.0))
    assert row is not None and row < 120
    assert bl.horizon_row(at_start(calib(), tilt=-60.0)) is None


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
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_detectors.py 2>&1 | tail -3`
Expected: an import error (`conqueror_ai.domain.balls` doesn't exist).

- [ ] **Step 3: Write the three modules.** `vocabulary.py`:

```python
"""The words YOLO-World looks for (AI engine LLD §16.1 item 2): config/vocabulary.yaml as a pure value."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

DEFAULT_MIN_SCORE = 0.25


@dataclass(frozen=True)
class Word:
    word: str
    floor: bool  # it stands on the floor: the floor trick places it
    min_score: float = DEFAULT_MIN_SCORE


@dataclass(frozen=True)
class Vocabulary:
    words: tuple[Word, ...]

    @staticmethod
    def from_dict(d: dict[str, Any]) -> Vocabulary:
        """{"words": [{"word": ..., "floor": ..., "min_score": ...}, ...]}; a word twice or no word is
        refused."""
        rows: list[dict[str, Any]] = list(d.get("words") or [])
        if not rows:
            raise ValueError("the vocabulary has no words")
        words = tuple(
            Word(str(r["word"]), bool(r.get("floor", True)), float(r.get("min_score", DEFAULT_MIN_SCORE)))
            for r in rows
        )
        names = [w.word for w in words]
        dup = sorted({n for n in names if names.count(n) > 1})
        if dup:
            raise ValueError(f"the vocabulary has {', '.join(dup)} more than once")
        return Vocabulary(words)

    @property
    def names(self) -> list[str]:
        return [w.word for w in self.words]

    @property
    def floor_classes(self) -> frozenset[str]:
        return frozenset(w.word for w in self.words if w.floor)

    def min_score(self, word: str) -> float:
        for w in self.words:
            if w.word == word:
                return w.min_score
        return DEFAULT_MIN_SCORE

    def check_model(self, model_names: list[str]) -> None:
        """A model exported for other words is refused: its baked-in names must be these, in this order."""
        if model_names != self.names:
            raise ValueError(
                "the detector model was made for other words "
                f"({', '.join(model_names)}) - re-export: cd tools/measure && uv run python export_world.py"
            )
```

`balls.py`:

```python
"""The colour ball finder (AI engine LLD §16.1 items 3 and 5): round blobs of known colours on the floor.

Plain OpenCV on arrays, no files: the largest round blob of each ball's colour below the horizon. Measured
on the
tank (design-notes §152): the green ball in 187 of 194 pictures, steady to +-1.6 px, 1.1 ms at VGA."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import cast

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import CameraModel
from conqueror_ai.domain.types import Detection

MIN_ROUND = 0.66  # convex hull area / (pi r^2) of the enclosing circle. The tank's green ball measured 0.73
#   (p10 0.70; its shaded side and pattern edge are not green), a square 0.64, a 2:3 box 0.59 (§152 / Plan Y)
SCORE_K = 1.3  # score = min(1, roundness x this): the ball ~0.95
HORIZON_AHEAD_M = 1000.0  # a floor point this far ahead lies on the horizon


@dataclass(frozen=True)
class BallColour:
    name: str
    h_lo: int  # OpenCV hue, 0-179
    h_hi: int
    s_min: int
    v_max: int
    r_min_px: float = 6.0


def horizon_row(cam: CameraModel) -> int | None:
    """The picture row of the horizon straight ahead of the camera, or None when it is off the picture."""
    c = cam.position()
    fwd = cam.axes()[2]  # right, down, forward
    n = math.hypot(float(fwd[0]), float(fwd[1]))
    if n < 1e-6:
        return None  # looking straight down
    x = float(c[0]) + float(fwd[0]) / n * HORIZON_AHEAD_M
    y = float(c[1]) + float(fwd[1]) / n * HORIZON_AHEAD_M
    q = cam.floor_to_pixel(x, y)
    if q is None:
        return None
    row = round(q[1])
    return row if 0 <= row < cam.cal.size[1] else None


U8 = NDArray[np.uint8]


def _hsv(rgb: U8) -> U8:
    return cast("U8", cv2.cvtColor(cv2.GaussianBlur(rgb, (5, 5), 0), cv2.COLOR_RGB2HSV))


def _in_range(hsv: U8, lo: tuple[int, int, int], hi: tuple[int, int, int], horizon: int | None) -> U8:
    """The pixels inside the HSV range, below the horizon, specks removed."""
    m = cast("U8", cv2.inRange(hsv, np.array(lo, np.uint8), np.array(hi, np.uint8)))
    if horizon is not None:
        m[: max(0, horizon)] = 0  # only the floor
    return cast("U8", cv2.morphologyEx(m, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8)))


def _mask(hsv: U8, b: BallColour, horizon: int | None) -> U8:
    return _in_range(hsv, (b.h_lo, b.s_min, 0), (b.h_hi, 255, b.v_max), horizon)


def _blob(mask: NDArray[np.uint8], r_min: float) -> tuple[float, float, float, float] | None:
    """The largest round blob: centre x, centre y, radius, roundness."""
    best: tuple[float, float, float, float] | None = None
    best_area = 0.0
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    for c in contours:
        area = float(cv2.contourArea(c))
        (x, y), r = cv2.minEnclosingCircle(c)
        if r < r_min or r <= 0:
            continue
        roundness = float(cv2.contourArea(cv2.convexHull(c))) / (math.pi * r * r)
        if roundness >= MIN_ROUND and area > best_area:
            best, best_area = (float(x), float(y), float(r), roundness), area
    return best


def find_balls(rgb: NDArray[np.uint8], balls: list[BallColour], horizon: int | None) -> list[Detection]:
    """One detection per ball colour found (the largest round blob of it), never a guess."""
    if not balls:
        return []
    hsv = _hsv(rgb)
    h, w = rgb.shape[:2]
    out: list[Detection] = []
    for b in balls:
        blob = _blob(_mask(hsv, b, horizon), b.r_min_px)
        if blob is None:
            continue
        x, y, r, roundness = blob
        box = (max(0.0, x - r), max(0.0, y - r), min(float(w), x + r), min(float(h), y + r))
        out.append(Detection(f"ball:{b.name}", min(1.0, roundness * SCORE_K), box))
    return out


def colour_from_samples(name: str, hsv_pixels: NDArray[np.uint8], r_min_px: float) -> BallColour:
    """A ball's colour range from its pixels (cq-calibrate ball): hue p5-p95 widened by 5, s_min = p10 - 10,
    v_max = p90 + 15 (design-notes §152 found the floor at S ~30, V 230-255 and the ball at S 72-108, V
    130-185)."""
    hsv = hsv_pixels.reshape(-1, 3).astype(np.float64)
    h5, h95 = np.percentile(hsv[:, 0], [5, 95])
    s10 = float(np.percentile(hsv[:, 1], 10))
    v90 = float(np.percentile(hsv[:, 2], 90))
    return BallColour(
        name,
        max(0, int(h5) - 5),
        min(179, math.ceil(h95) + 5),
        max(0, int(s10) - 10),
        min(255, math.ceil(v90) + 15),
        r_min_px,
    )


def most_coloured_blob(rgb: NDArray[np.uint8], horizon: int | None) -> NDArray[np.uint8] | None:
    """For calibration: the HSV pixels of the most saturated round blob below the horizon (the ball laid out),
    or None. Saturation >= 60 on a blurred picture, the largest round piece."""
    hsv = _hsv(rgb)
    m = _in_range(hsv, (0, 60, 0), (179, 255, 255), horizon)
    blob = _blob(m, 6.0)
    if blob is None:
        return None
    x, y, r, _ = blob
    disc = np.zeros(m.shape, np.uint8)
    cv2.circle(disc, (round(x), round(y)), max(1, round(r * 0.7)), (255,), -1)  # the middle: no edge pixels
    pix: U8 = hsv[(disc > 0) & (m > 0)]
    return pix if len(pix) else None
```

`detectors.py`:

```python
"""Several detectors as one (AI engine LLD §16.1 item 4): YOLO-World and the colour ball finder on one
picture."""

from __future__ import annotations

from collections.abc import Callable, Sequence

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.types import Detection

SAME_BALL_IOU = 0.5

Box = tuple[float, float, float, float]
DetectFn = Callable[[NDArray[np.uint8], "int | None"], list[Detection]]


def iou(a: Box, b: Box) -> float:
    ix = max(0.0, min(a[2], b[2]) - max(a[0], b[0]))
    iy = max(0.0, min(a[3], b[3]) - max(a[1], b[1]))
    inter = ix * iy
    union = (a[2] - a[0]) * (a[3] - a[1]) + (b[2] - b[0]) * (b[3] - b[1]) - inter
    return inter / union if union > 0 else 0.0


class MultiDetector:
    """Runs each detector on the same picture and joins the lists. Any other box on a colour ball (IoU >= 0.5)
    is that ball, whatever its word (YOLO-World called the green ball "toy"): dropped - the colour finder is
    the steadier one, and one ball must be one thing."""

    def __init__(self, detectors: Sequence[DetectFn]) -> None:
        self._detectors = list(detectors)

    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None) -> list[Detection]:
        out: list[Detection] = []
        for d in self._detectors:
            out.extend(d(rgb, horizon_row))
        colour = [d for d in out if d.cls.startswith("ball:")]
        return [
            d
            for d in out
            if d.cls.startswith("ball:") or not any(iou(d.box, c.box) >= SAME_BALL_IOU for c in colour)
        ]
```

- [ ] **Step 4: Run the tests, ruff and pyright**

Run: `cd ai-engine && uv run pytest -q tests/test_detectors.py && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `16 passed`; all clean.

- [ ] **Step 5: Commit** — `feat(ai): the words, the colour ball finder and the joined detectors (AI engine LLD §16.1)`

---

### Task 2: YOLO-World, exported and wrapped

**Files:**
- Create:
  - `tools/measure/export_world.py`
  - `ai-engine/src/conqueror_ai/adapters/yolo_world.py`
  - `ai-engine/tests/test_yolo_world.py`
- Modify:
  - `tools/measure/pyproject.toml` (+ `uv.lock`)
  - `ai-engine/config/vocabulary.yaml` (created here; Task 3 reads it)

**Interfaces:**
- Consumes: `Vocabulary` (Task 1); `letterbox` from `adapters/yolo_coreml.py` (§15.1).
- Produces: `YoloWorld(path, vocab).detect(rgb, horizon_row)`; `yolo_world.to_detections(raw, pad, size, vocab)`; the model `ai-engine/models/yoloworld-s-640.mlpackage`.

- [ ] **Step 1: The words file**, `ai-engine/config/vocabulary.yaml`:

```yaml
# The words YOLO-World looks for (AI engine LLD §16.1 item 2). floor: true = it stands on the floor (the floor
# trick places it). min_score: its own threshold (0.25 unless set). After a change re-export the model:
#   cd tools/measure && uv run python export_world.py
words:
  - {word: person, floor: true}
  - {word: chair, floor: true}
  - {word: table, floor: true}
  - {word: bed, floor: true}
  - {word: sofa, floor: true}
  - {word: door, floor: false}
  - {word: bottle, floor: true}
  - {word: shoe, floor: true}
  - {word: box, floor: true}
  - {word: toy, floor: true}
```

- [ ] **Step 2: Ultralytics' CLIP in `tools/measure`.**
  - Add to its `dependencies`: `"clip @ git+https://github.com/ultralytics/CLIP.git",  # YOLO-World's words, only for export_world.py (LLD §16)`.
  - Then run `cd tools/measure && uv lock`. Expected: the lock gains `clip 1.0` from that git source (the owner allowed this install, design-notes §152).

- [ ] **Step 3: `tools/measure/export_world.py`**:

```python
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
```
Run: `cd tools/measure && uv run python export_world.py`
Expected:
- it downloads `yolov8s-worldv2.pt` (~25 MB) once, and CLIP's ~338 MB weights on the first `set_classes`;
- `detector model: …/ai-engine/models/yoloworld-s-640.mlpackage (10 words: person, chair, …, toy)`.

- [ ] **Step 4: The adapter's tests**, `ai-engine/tests/test_yolo_world.py`:

```python
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

    det = yw.YoloWorld(
        MODEL, load_vocabulary(Path(__file__).resolve().parents[1] / "config" / "vocabulary.yaml")
    )
    out = det.detect(np.full((480, 640, 3), 128, dtype=np.uint8), 240)
    for d in out:
        x1, y1, x2, y2 = d.box
        assert 0 <= x1 <= x2 <= 640 and 0 <= y1 <= y2 <= 480
```
Run: `cd ai-engine && uv run pytest -q tests/test_yolo_world.py` → fails: no module `yolo_world`.

- [ ] **Step 5: The adapter**, `ai-engine/src/conqueror_ai/adapters/yolo_world.py`:

```python
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
    """The model's raw output (4 + words, candidates) -> detections in the picture's pixels."""
    top, left, scale = pad
    w, h = size
    a = raw.reshape(raw.shape[-2], raw.shape[-1])
    cx, cy, bw, bh = a[0], a[1], a[2], a[3]
    scores = a[4:]
    out: list[Detection] = []
    for k, word in enumerate(vocab.names):
        sel = scores[k] >= vocab.min_score(word)
        if not sel.any():
            continue
        boxes = np.stack(
            [cx[sel] - bw[sel] / 2, cy[sel] - bh[sel] / 2, cx[sel] + bw[sel] / 2, cy[sel] + bh[sel] / 2],
            axis=1,
        )
        sc = scores[k][sel].astype(np.float64)
        for i in _nms(boxes.astype(np.float64), sc):
            x1, y1, x2, y2 = boxes[i]
            box = (
                min(max((x1 - left) / scale, 0.0), float(w)),
                min(max((y1 - top) / scale, 0.0), float(h)),
                min(max((x2 - left) / scale, 0.0), float(w)),
                min(max((y2 - top) / scale, 0.0), float(h)),
            )
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

    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None) -> list[Detection]:
        from PIL import Image

        img, pad = letterbox(rgb, self._size)
        out: dict[str, Any] = self._model.predict({self._input: Image.fromarray(img)})
        raw = np.asarray(next(iter(out.values())), dtype=np.float64)
        return to_detections(raw, pad, (rgb.shape[1], rgb.shape[0]), self._vocab)
```

- [ ] **Step 6: Run the tests, then check the adapter against Ultralytics.**
  - Run: `cd ai-engine && uv run pytest -q tests/test_yolo_world.py`. Expected: 5 passed (the real-model test runs, because the model now exists).
  - On COCO val picture 139 (`http://images.cocodataset.org/val2017/000000000139.jpg`, downloaded into its own scratch folder), compare our `YoloWorld` with Ultralytics' `YOLOWorld.predict(conf=0.25, imgsz=640)` with the same words.
  - Expected: the same person 0.83 and chairs 0.76 / 0.58 / 0.54, boxes within 1 px.
  - Time 30 pictures: median ≈ 6 ms (≤ 30 ms, LLD §16.4).

- [ ] **Step 7: Lint and commit** — `feat(ai): YOLO-World with the words baked in, on CoreML (AI engine LLD §16.1 item 1)`

---

### Task 3: The plug, the config, VGA and cq-objects

**Files:**
- Create:
  - `ai-engine/src/conqueror_ai/adapters/detector_config.py`
  - `ai-engine/config/balls.yaml`
- Modify:
  - `ai-engine/src/conqueror_ai/domain/objects.py`
  - `ai-engine/src/conqueror_ai/adapters/calibration_store.py`
  - `ai-engine/src/conqueror_ai/adapters/yolo_coreml.py`
  - `ai-engine/src/conqueror_ai/app/objects.py`
  - `ai-engine/tests/test_calibration_files.py`
  - `ai-engine/tests/test_objects_app.py`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces:
  - `load_vocabulary(path)`, `load_balls(path)`, `save_ball(path, ball)`;
  - `ObjectPipeline(cal, floor_classes)` and `.camera_at(cap_us)`;
  - `ObjectLocator(floor_classes)`;
  - `run_replay(folder, cal, det, floor_classes)`;
  - `objects.detectors(model, vocab_path, balls_path) -> (Detector, floor_classes)`;
  - `horizon_of(p, frame)`.

- [ ] **Step 1: The tests.** Apply these diffs.
  - The size test now allows VGA and refuses 800 × 600, and a VGA test is added:

```diff
diff --git a/ai-engine/tests/test_calibration_files.py b/ai-engine/tests/test_calibration_files.py
index 43f1f48..8b9b375 100644
--- a/ai-engine/tests/test_calibration_files.py
+++ b/ai-engine/tests/test_calibration_files.py
@@ -28,7 +28,28 @@ def test_a_file_for_another_picture_size_is_refused(tmp_path: Path) -> None:
     path = tmp_path / "calibration.yaml"
     calibration_store.save(calib(), path)
     with pytest.raises(ValueError, match="320 x 240"):
-        calibration_store.load(path, size=(640, 480))
+        calibration_store.load(path, size=(800, 600))
+
+
+def test_vga_is_the_qvga_file_at_twice_the_pixels(tmp_path: Path) -> None:
+    """LLD §16.1 item 6: the same field of view, so a floor point lands on twice the pixel."""
+    import math
+
+    from conqueror_ai.domain.camera import CameraModel
+    from conqueror_ai.domain.types import Pose
+
+    path = tmp_path / "calibration.yaml"
+    calibration_store.save(calib(), path)
+    q, v = calibration_store.load(path, (320, 240)), calibration_store.load(path, (640, 480))
+    assert v.size == (640, 480) and v.k[0, 0] == 2 * q.k[0, 0] and v.k[1, 1] == 2 * q.k[1, 1]
+    pose = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)
+    cq, cv = CameraModel(q).for_frame(pose, 0.0, -15.0), CameraModel(v).for_frame(pose, 0.0, -15.0)
+    for x, y in ((1.0, 0.0), (0.6, 0.2), (2.0, -0.4)):
+        a, b = cq.floor_to_pixel(x, y), cv.floor_to_pixel(x, y)
+        assert a is not None and b is not None
+        assert math.isclose((a[0] + 0.5) * 2 - 0.5, b[0], abs_tol=1e-6) and math.isclose(
+            (a[1] + 0.5) * 2 - 0.5, b[1], abs_tol=1e-6
+        )
 
 
 def test_the_checkerboard_picture_and_its_corners_are_found_in_perspective(tmp_path: Path) -> None:
```

  - The fake detector takes the horizon, and a VGA replay test is added (a fake YOLO-World with "toy" on the ball and a chair; the real colour finder):

```diff
diff --git a/ai-engine/tests/test_objects_app.py b/ai-engine/tests/test_objects_app.py
index 48abb10..bd69de7 100644
--- a/ai-engine/tests/test_objects_app.py
+++ b/ai-engine/tests/test_objects_app.py
@@ -26,7 +26,7 @@ class BallDetector:
         self.box = box_on_floor(cam_at(), 1.0, 0.1)
         self.calls = 0
 
-    def detect(self, rgb: NDArray[np.uint8]) -> list[Detection]:
+    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None) -> list[Detection]:
         assert rgb.shape == (240, 320, 3)
         self.calls += 1
         return [self.box]
@@ -97,3 +97,45 @@ def test_the_live_view_takes_boxes_and_the_plan(tmp_path: Path) -> None:
     view.plan(1_000_000, Pose(1_000_000, 0.0, 0.0, 0.0, 0.0, 0.0), [seen, gone], None)
     view.plan(1_100_000, None, [], "turning")
     assert (tmp_path / "v.rrd").exists()
+
+
+def test_a_vga_recording_lists_the_colour_ball_once_with_a_word_box_on_it(tmp_path: Path) -> None:
+    """LLD §16.5: a fake YOLO-World ("toy" on the ball, a chair elsewhere) and the real colour finder -> one
+    ball."""
+    import cv2
+
+    from conqueror_ai.adapters import calibration_store
+    from conqueror_ai.domain.balls import BallColour, find_balls
+    from conqueror_ai.domain.camera import CameraModel
+    from conqueror_ai.domain.detectors import MultiDetector
+    from conqueror_ai.domain.types import Pose
+    from tests.test_camera import calib
+
+    cfile = tmp_path / "calibration.yaml"
+    calibration_store.save(calib(), cfile)
+    cal = calibration_store.load(cfile, (640, 480))
+    cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
+    q = cam.floor_to_pixel(1.0, 0.1)  # the ball's bottom, 1 m ahead
+    assert q is not None
+    u, v = round(q[0]), round(q[1])
+    hsv = np.zeros((480, 640, 3), np.uint8)
+    hsv[:] = (55, 30, 240)
+    pic = cv2.cvtColor(hsv, cv2.COLOR_HSV2BGR)
+    green = cv2.cvtColor(np.array([[(64, 90, 170)]], np.uint8), cv2.COLOR_HSV2BGR)[0, 0]
+    cv2.circle(pic, (u, v - 14), 14, tuple(int(c) for c in green), -1)
+    jpeg = bytes(cv2.imencode(".jpg", pic, [cv2.IMWRITE_JPEG_QUALITY, 95])[1].tobytes())
+    rec = SessionRecorder(tmp_path / "s", {"what": "test"})
+    for t in range(0, 2000, 20):
+        rec.telemetry(t / 1000, td(t))
+        if t % 100 == 0 and t >= 100:
+            rec.frame(Frame(seq=t, cap_us=(t - 100) * 1000, recv_s=t / 1000, jpeg=jpeg))
+    rec.close()
+    toy = Detection("toy", 0.4, (u - 15.0, v - 29.0, u + 15.0, v + 1.0))
+    chair = Detection("chair", 0.8, (100.0, 220.0, 160.0, 330.0))  # ~1.2 m away, left
+    green_ball = BallColour("green", 45, 90, 65, 200, 6.0)
+    det = MultiDetector([lambda rgb, h: [toy, chair], lambda rgb, h: find_balls(rgb, [green_ball], h)])
+    res = app.run_replay(tmp_path / "s", cal, det, frozenset({"toy", "chair", "ball:green"}))
+    names = sorted(t.cls for t in res.things.values())
+    assert names == ["ball:green", "chair"]
+    ball = next(t for t in res.things.values() if t.cls == "ball:green")
+    assert math.isclose(ball.x_m, 1.0, abs_tol=0.05) and math.isclose(ball.y_m, 0.1, abs_tol=0.05)
```
Run: `cd ai-engine && uv run pytest -q tests/test_calibration_files.py tests/test_objects_app.py` → fails (VGA refused; `run_replay` has no `floor_classes`; the fake's signature).

- [ ] **Step 2: The code.** `ai-engine/config/balls.yaml`:

```yaml
# Colour balls for the ball finder (AI engine LLD §16.1 item 3); written by cq-calibrate ball <name>.
# green: the owner's ball, measured at camera setting B on 2026-10-24 (design-notes §152).
balls:
- {name: green, h: [45, 90], s_min: 65, v_max: 200, r_min_px: 6.0}
```
`adapters/detector_config.py`:

```python
"""The detector's config files (AI engine LLD §16.1): config/vocabulary.yaml (the words) and config/balls.yaml
(the colour balls). Plain YAML; the values are checked by the domain types."""

from __future__ import annotations

from dataclasses import asdict
from pathlib import Path
from typing import Any

import yaml

from conqueror_ai.domain.balls import BallColour
from conqueror_ai.domain.vocabulary import Vocabulary


def load_vocabulary(path: Path) -> Vocabulary:
    d: dict[str, Any] = yaml.safe_load(path.read_text()) or {}
    return Vocabulary.from_dict(d)


def load_balls(path: Path) -> list[BallColour]:
    if not path.exists():
        return []
    d: dict[str, Any] = yaml.safe_load(path.read_text()) or {}
    out: list[BallColour] = []
    rows: list[dict[str, Any]] = list(d.get("balls") or [])
    for r in rows:
        h: list[Any] = list(r["h"])
        out.append(
            BallColour(
                str(r["name"]),
                int(h[0]),
                int(h[1]),
                int(r["s_min"]),
                int(r["v_max"]),
                float(r.get("r_min_px", 6.0)),
            )
        )
    return out


def save_ball(path: Path, ball: BallColour) -> None:
    """Adds the ball, or replaces the one of the same name; the others are kept."""
    balls = [b for b in load_balls(path) if b.name != ball.name] + [ball]
    rows: list[dict[str, Any]] = []
    for b in balls:
        r = asdict(b)
        rows.append(
            {
                "name": r["name"],
                "h": [r["h_lo"], r["h_hi"]],
                "s_min": r["s_min"],
                "v_max": r["v_max"],
                "r_min_px": r["r_min_px"],
            }
        )
    head = (
        "# Colour balls for the ball finder (AI engine LLD §16.1 item 3);"
        " written by cq-calibrate ball <name>.\n"
    )
    path.write_text(head + yaml.safe_dump({"balls": rows}, sort_keys=False))
```
and these diffs:

```diff
diff --git a/ai-engine/src/conqueror_ai/adapters/calibration_store.py b/ai-engine/src/conqueror_ai/adapters/calibration_store.py
index 649ea62..17e3bf2 100644
--- a/ai-engine/src/conqueror_ai/adapters/calibration_store.py
+++ b/ai-engine/src/conqueror_ai/adapters/calibration_store.py
@@ -8,6 +8,7 @@ from typing import Any
 
 import numpy as np
 import yaml
+from numpy.typing import NDArray
 
 from conqueror_ai.domain.camera import Calibration
 
@@ -31,17 +32,32 @@ def save(cal: Calibration, path: Path) -> None:
     path.write_text(yaml.safe_dump(doc, sort_keys=False))
 
 
+def _scaled_k(k: NDArray[np.float64], scale: float) -> NDArray[np.float64]:
+    """fx, fy, cx, cy times the scale (pixel centres: cx' = (cx + 0.5) x s - 0.5)."""
+    if scale == 1.0:
+        return k
+    out = k.copy()
+    out[0, 0] *= scale
+    out[1, 1] *= scale
+    out[0, 2] = (k[0, 2] + 0.5) * scale - 0.5
+    out[1, 2] = (k[1, 2] + 0.5) * scale - 0.5
+    return out
+
+
 def load(path: Path, size: tuple[int, int]) -> Calibration:
     """The calibration for pictures of `size`; ValueError when the file is for another size."""
     d: dict[str, Any] = yaml.safe_load(path.read_text())
     got = (int(d["size"][0]), int(d["size"][1]))
+    scale = 1.0
     if got != size:
-        raise ValueError(f"{path} is for {got[0]} x {got[1]} pictures, not {size[0]} x {size[1]}")
+        if size != (got[0] * 2, got[1] * 2):
+            raise ValueError(f"{path} is for {got[0]} x {got[1]} pictures, not {size[0]} x {size[1]}")
+        scale = 2.0  # VGA from the QVGA file: the same field of view, twice the pixels (LLD §16.1 item 6)
     m, s = d["mount"], d["servos"]
     return Calibration(
-        k=np.asarray(d["lens"]["k"], dtype=np.float64),
+        k=_scaled_k(np.asarray(d["lens"]["k"], dtype=np.float64), scale),
         dist=np.asarray(d["lens"]["dist"], dtype=np.float64),
-        size=got,
+        size=size,
         cam_height_m=float(m["cam_height_m"]),
         cam_x_m=float(m["cam_x_m"]),
         cam_y_m=float(m["cam_y_m"]),
diff --git a/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py b/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py
index 0b54c31..7b85819 100644
--- a/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py
+++ b/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py
@@ -76,7 +76,7 @@ class YoloCoreml:
         self._output = str(spec.description.output[0].name)
         self._conf = conf
 
-    def detect(self, rgb: NDArray[np.uint8]) -> list[Detection]:
+    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None = None) -> list[Detection]:
         from PIL import Image
 
         img, pad = letterbox(rgb, self._size)
diff --git a/ai-engine/src/conqueror_ai/app/objects.py b/ai-engine/src/conqueror_ai/app/objects.py
index 13f2ee0..47c44f1 100644
--- a/ai-engine/src/conqueror_ai/app/objects.py
+++ b/ai-engine/src/conqueror_ai/app/objects.py
@@ -1,5 +1,8 @@
-"""`cq-objects`: the things in the room (AI engine LLD §15.6) - phase 4 of the map. It only watches; it never
-drives.
+"""`cq-objects`: the things in the room (AI engine LLD §15.6, §16) - phase 4 of the map. It only watches; it
+never
+drives. What spots things: YOLO-World asked for the words in config/vocabulary.yaml, and the colour ball
+finder for
+the balls in config/balls.yaml (§16); the camera at VGA.
 
     uv run cq-objects recordings/20261024-101500      # replay: every picture, in order; prints what was found
     uv run cq-objects --live                          # the tank: the live view (picture + boxes, the plan)
@@ -22,14 +25,19 @@ from numpy.typing import NDArray
 
 from conqueror_ai.adapters import calibration_store
 from conqueror_ai.adapters.session import replay
+from conqueror_ai.domain.balls import BallColour, find_balls, horizon_row
 from conqueror_ai.domain.camera import Calibration
-from conqueror_ai.domain.objects import Detector, ObjectPipeline
+from conqueror_ai.domain.detectors import MultiDetector
+from conqueror_ai.domain.objects import FLOOR_CLASSES, Detector, ObjectPipeline
 from conqueror_ai.domain.telemetry import parse_td
 from conqueror_ai.domain.types import Frame, ObjectEstimate
 
 ENGINE = Path(__file__).resolve().parents[3]
 CALIBRATION = ENGINE / "config" / "calibration.yaml"
-MODEL = ENGINE / "models" / "yolo26n-320.mlpackage"
+MODEL = ENGINE / "models" / "yoloworld-s-640.mlpackage"
+VOCABULARY = ENGINE / "config" / "vocabulary.yaml"
+BALLS = ENGINE / "config" / "balls.yaml"
+VGA = (640, 480)
 DEFAULT_HOST = "192.168.31.167"
 
 
@@ -68,9 +76,17 @@ def _note(res: ReplayResult, objs: list[ObjectEstimate]) -> None:
         res.things[o.oid] = Thing(o.cls, o.first_seen_us, o.last_seen_us, o.x_m, o.y_m, o.sigma_m)
 
 
-def run_replay(folder: Path, cal: Calibration, det: Detector) -> ReplayResult:
+def horizon_of(p: ObjectPipeline, f: Frame) -> int | None:
+    """The picture's horizon row from its camera (the ball finder looks below it); None if unknown."""
+    cam = p.camera_at(f.cap_us)
+    return None if cam is None else horizon_row(cam)
+
+
+def run_replay(
+    folder: Path, cal: Calibration, det: Detector, floor_classes: frozenset[str] = FLOOR_CLASSES
+) -> ReplayResult:
     """Every recorded picture, in order, through the detector and the list (the same answer every time)."""
-    p = ObjectPipeline(cal)
+    p = ObjectPipeline(cal, floor_classes)
     res = ReplayResult()
     for e in replay(folder):
         if e.text is not None:
@@ -87,7 +103,7 @@ def run_replay(folder: Path, cal: Calibration, det: Detector) -> ReplayResult:
             rgb = decode(e.frame)
             if rgb is None:
                 continue
-            cam = p.on_frame(e.frame, (rgb.shape[1], rgb.shape[0]), det.detect(rgb))
+            cam = p.on_frame(e.frame, (rgb.shape[1], rgb.shape[0]), det.detect(rgb, horizon_of(p, e.frame)))
             if not isinstance(cam, str):
                 res.used += 1
                 _note(res, p.objects())
@@ -109,7 +125,9 @@ def report(res: ReplayResult) -> str:
     return "\n".join(lines)
 
 
-async def run_live(host: str, cal: Calibration, det: Detector) -> None:
+async def run_live(
+    host: str, cal: Calibration, det: Detector, floor_classes: frozenset[str] = FLOOR_CLASSES
+) -> None:
     """The tank: telemetry and the newest picture; the detector in a worker thread (latest picture wins)."""
     from conqueror_ai.adapters.bridge_ws import BridgeLink
     from conqueror_ai.adapters.mjpeg_source import MjpegSource
@@ -117,7 +135,7 @@ async def run_live(host: str, cal: Calibration, det: Detector) -> None:
 
     link, cam = BridgeLink(host), MjpegSource(host)
     view = RerunView(spawn=True)
-    p = ObjectPipeline(cal)
+    p = ObjectPipeline(cal, floor_classes)
     stop = asyncio.Event()
     tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]
 
@@ -149,7 +167,7 @@ async def run_live(host: str, cal: Calibration, det: Detector) -> None:
             if rgb is None:
                 continue
             dets = await asyncio.to_thread(
-                det.detect, rgb
+                det.detect, rgb, horizon_of(p, f)
             )  # meanwhile newer pictures replace the waiting one
             used = p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets)
             view.frame(f, None)
@@ -164,6 +182,18 @@ async def run_live(host: str, cal: Calibration, det: Detector) -> None:
             await asyncio.gather(*tasks, return_exceptions=True)
 
 
+def detectors(model: Path, vocab_path: Path, balls_path: Path) -> tuple[Detector, frozenset[str]]:
+    """YOLO-World and the colour ball finder as one detector, and the classes the floor trick places (§16)."""
+    from conqueror_ai.adapters.detector_config import load_balls, load_vocabulary
+    from conqueror_ai.adapters.yolo_world import YoloWorld
+
+    vocab = load_vocabulary(vocab_path)
+    world = YoloWorld(model, vocab)
+    balls: list[BallColour] = load_balls(balls_path)
+    multi = MultiDetector([world.detect, lambda rgb, h: find_balls(rgb, balls, h)])
+    return multi, vocab.floor_classes | frozenset(f"ball:{b.name}" for b in balls)
+
+
 def main() -> None:
     ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
     ap.add_argument("folder", type=Path, nargs="?")
@@ -171,21 +201,21 @@ def main() -> None:
     ap.add_argument("--host", default=DEFAULT_HOST)
     ap.add_argument("--calibration", type=Path, default=CALIBRATION)
     ap.add_argument("--model", type=Path, default=MODEL)
+    ap.add_argument("--vocabulary", type=Path, default=VOCABULARY)
+    ap.add_argument("--balls", type=Path, default=BALLS)
     a = ap.parse_args()
     if a.live == (a.folder is not None):
         raise SystemExit("give a recording folder, or --live")
-    from conqueror_ai.adapters.yolo_coreml import YoloCoreml
-
     try:
-        cal = calibration_store.load(a.calibration, (320, 240))
-        det = YoloCoreml(a.model)
-    except (FileNotFoundError, ValueError) as err:
+        cal = calibration_store.load(a.calibration, VGA)
+        det, floor = detectors(a.model, a.vocabulary, a.balls)
+    except (FileNotFoundError, ValueError, KeyError) as err:
         raise SystemExit(str(err)) from None
     if a.live:
         with contextlib.suppress(KeyboardInterrupt):
-            asyncio.run(run_live(a.host, cal, det))
+            asyncio.run(run_live(a.host, cal, det, floor))
     else:
-        print(report(run_replay(a.folder, cal, det)))
+        print(report(run_replay(a.folder, cal, det, floor)))
 
 
 if __name__ == "__main__":
diff --git a/ai-engine/src/conqueror_ai/domain/objects.py b/ai-engine/src/conqueror_ai/domain/objects.py
index 79ae1c9..de7f49b 100644
--- a/ai-engine/src/conqueror_ai/domain/objects.py
+++ b/ai-engine/src/conqueror_ai/domain/objects.py
@@ -64,9 +64,12 @@ FloatArray = NDArray[np.float64]
 
 
 class Detector(Protocol):
-    """The detector port (LLD §15.1): YoloCoreml on the tank's pictures, a fake in tests."""
+    """The detector port (LLD §15.1, §16): YOLO-World and the colour ball finder on the tank's pictures, a
+    fake in
+    tests. horizon_row: the picture's horizon (the ball finder looks only below it), None when off the
+    picture."""
 
-    def detect(self, rgb: NDArray[np.uint8]) -> list[Detection]: ...
+    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None) -> list[Detection]: ...
 
 
 def sigma_at(d: float) -> float:
@@ -148,13 +151,16 @@ class Near:
 
 
 class ObjectLocator:
+    def __init__(self, floor_classes: frozenset[str] = FLOOR_CLASSES) -> None:
+        self.floor_classes = floor_classes  # LLD §16: the vocabulary's floor words and the colour balls
+
     def locate(self, dets: list[Detection], cam: CameraModel, t_us: int) -> tuple[list[Located], list[Near]]:
         located: list[Located] = []
         near: list[Near] = []
         h = cam.cal.size[1]
         c = cam.position()
         for d in dets:
-            if d.cls not in FLOOR_CLASSES:
+            if d.cls not in self.floor_classes:
                 continue
             x1, y1, x2, y2 = d.box
             u = (x1 + x2) / 2.0
@@ -312,13 +318,13 @@ class ObjectTracker:
 class ObjectPipeline:
     """Telemetry and pictures in arrival order -> the list. The detector is run outside and handed in."""
 
-    def __init__(self, cal: Calibration) -> None:
+    def __init__(self, cal: Calibration, floor_classes: frozenset[str] = FLOOR_CLASSES) -> None:
         self.cal = cal
         self._pose = PoseFilter()
         self._history = PoseHistory()
         self._angles = CameraAngles()
         self.gate = FrameGate(cal, self._history, self._angles)
-        self._locator = ObjectLocator()
+        self._locator = ObjectLocator(floor_classes)
         self.tracker = ObjectTracker()
         self._near: list[Near] = []
         self._skips: Counter[str] = Counter()
@@ -332,6 +338,11 @@ class ObjectPipeline:
         self._history.push(self._pose.update(s))
         self._angles.push(s)
 
+    def camera_at(self, cap_us: int) -> CameraModel | None:
+        """The camera for a picture before detecting (for its horizon); None when the gate would skip it."""
+        cam = self.gate.camera_for(cap_us)
+        return None if isinstance(cam, str) else cam
+
     def on_frame(self, f: Frame, size: tuple[int, int], dets: list[Detection]) -> CameraModel | str:
         """Use one picture (its decoded size, its detections): its camera, or why it was skipped."""
         if self._last_cap_us is not None and f.cap_us < self._last_cap_us:
```

- [ ] **Step 3: Run everything**

Run: `cd ai-engine && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: about 164 passed, 1 skipped; all clean.

- [ ] **Step 4: A real recording.** Nothing moves.
  - Setup: the camera at setting B and VGA; the green ball ~1 m ahead; a chair further away.
  - Run: `cd ai-engine && uv run cq-record --seconds 6 --no-view --out recordings/y-ball && uv run cq-objects recordings/y-ball`.
  - Expected: one `ball:green` at ~106 cm ahead of the tank's centre (~1 m from the lens); no "toy" on it; the chair listed.
  - Time both detectors on 60 of its pictures: median ≈ 7 ms.
  - Delete the recording.

- [ ] **Step 5: Commit** — `feat(ai): cq-objects with YOLO-World and the colour ball finder at VGA (AI engine LLD §16.1)`

---

### Task 4: `cq-calibrate ball <name>`

**Files:**
- Modify: `ai-engine/src/conqueror_ai/app/calibrate.py`
- Create: `ai-engine/tests/test_ball_calibrator.py`

**Interfaces:**
- Consumes: `most_coloured_blob`, `colour_from_samples` (Task 1); `save_ball` (Task 3).
- Produces: `calibrate.ball_colour(name, grab, n=15) -> BallColour`; the `ball` step.

- [ ] **Step 1: The tests**, `ai-engine/tests/test_ball_calibrator.py`:

```python
"""`cq-calibrate ball <name>` (AI engine LLD §16.1 item 5): the ball's colour from a few pictures, written to
config/balls.yaml beside the others. Drawn pictures and a fake camera."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters.detector_config import load_balls, save_ball
from conqueror_ai.app import calibrate
from conqueror_ai.domain import balls as bl
from tests.test_detectors import disc, floor_picture


async def test_the_ball_colour_comes_from_the_pictures_and_finds_that_ball() -> None:
    pics = [disc(floor_picture(640, 480), (330, 360), 24, (64, 90, 170)) for _ in range(15)]
    it = iter(pics)

    async def grab():  # type: ignore[no-untyped-def]
        return next(it)

    c = await calibrate.ball_colour("green", grab, n=15)
    assert c.name == "green" and c.h_lo <= 59 and c.h_hi >= 69
    assert len(bl.find_balls(pics[0], [c], horizon=240)) == 1
    assert bl.find_balls(floor_picture(640, 480), [c], horizon=240) == []


async def test_no_ball_in_view_is_a_plain_refusal() -> None:
    async def grab():  # type: ignore[no-untyped-def]
        return floor_picture(640, 480)

    with pytest.raises(calibrate.Refused, match="no coloured ball"):
        await calibrate.ball_colour("green", grab, n=5)


def test_saving_a_ball_keeps_the_others_and_replaces_its_own(tmp_path: Path) -> None:
    path = tmp_path / "balls.yaml"
    save_ball(path, bl.BallColour("green", 45, 90, 65, 200, 6.0))
    save_ball(path, bl.BallColour("red", 0, 8, 90, 230, 6.0))
    save_ball(path, bl.BallColour("green", 50, 80, 70, 190, 6.0))
    got = {b.name: b for b in load_balls(path)}
    assert set(got) == {"green", "red"} and got["green"].h_lo == 50 and got["red"].s_min == 90
```
Run: fails (`calibrate.ball_colour` doesn't exist).

- [ ] **Step 2: The step.** Apply:

```diff
diff --git a/ai-engine/src/conqueror_ai/app/calibrate.py b/ai-engine/src/conqueror_ai/app/calibrate.py
index a4b0369..b11a830 100644
--- a/ai-engine/src/conqueror_ai/app/calibrate.py
+++ b/ai-engine/src/conqueror_ai/app/calibrate.py
@@ -5,8 +5,10 @@
   servos  commanded -> real tilt and pan gains: a laser cross line of any colour, roughly at the lens height
           (design-notes §130); tilt0 and the pan centre are kept
   study   the blind zone and the view at a few tilts, for the owner's driving-tilt decision
+  ball    a ball ~1 m ahead in the middle of the view -> its colour range in config/balls.yaml (LLD §16)
 
-Each step saves config/calibration.yaml. The tool's only command to the tank is cam_set (bridge LLD R22): it
+Each step saves config/calibration.yaml (ball: config/balls.yaml). The tool's only command to the tank is
+cam_set (bridge LLD R22): it
 never drives. The steps take their inputs as functions, so tests drive them with a synthetic tank."""
 
 from __future__ import annotations
@@ -27,11 +29,15 @@ from numpy.typing import NDArray
 
 from conqueror_ai.adapters import calibration_store, lens_opencv
 from conqueror_ai.adapters.bridge_ws import BridgeLink, CommandRefused
+from conqueror_ai.adapters.detector_config import save_ball
 from conqueror_ai.adapters.mjpeg_source import MjpegSource
+from conqueror_ai.domain import balls as bl
 from conqueror_ai.domain import calib_math as cm
 from conqueror_ai.domain.camera import Calibration, CameraModel
 from conqueror_ai.domain.types import Pose
 
+BALLS = Path(__file__).resolve().parents[3] / "config" / "balls.yaml"
+
 Grab = Callable[[], Awaitable[NDArray[np.uint8]]]  # the newest picture, RGB
 Ask = Callable[[str], Awaitable[str]]  # a question to the owner, the typed answer
 CamSet = Callable[
@@ -320,7 +326,26 @@ async def _click(img: NDArray[np.uint8], what: str) -> tuple[float, float]:
     return got[0]
 
 
-async def _run(step: str, host: str, out: Path) -> None:
+async def ball_colour(
+    name: str, grab: Callable[[], Awaitable[NDArray[np.uint8]]], n: int = 15
+) -> bl.BallColour:
+    """The ball's colour (LLD §16.1 item 5): the most coloured round blob in the lower half of n pictures
+    (the ball
+    is put ~1 m ahead in the middle of the view), its middle pixels pooled -> the range."""
+    pix: list[NDArray[np.uint8]] = []
+    for _ in range(n):
+        rgb = await grab()
+        p = bl.most_coloured_blob(rgb, horizon=rgb.shape[0] // 2)
+        if p is not None:
+            pix.append(p)
+    if len(pix) < max(1, n // 2):
+        raise Refused(
+            f"no coloured ball found in {n - len(pix)} of {n} pictures - put it ~1 m ahead, in the middle"
+        )
+    return bl.colour_from_samples(name, np.concatenate(pix), r_min_px=6.0)
+
+
+async def _run(step: str, host: str, out: Path, ball: str = "") -> None:
     link = BridgeLink(host)
     cam = MjpegSource(host)
     stop = asyncio.Event()
@@ -343,6 +368,14 @@ async def _run(step: str, host: str, out: Path) -> None:
         await asyncio.sleep(1.0)  # the servos move at 120 deg/s; then a new picture
 
     try:
+        if step == "ball":
+            c = await ball_colour(ball, grab)
+            save_ball(BALLS, c)
+            print(
+                f"ball {c.name}: hue {c.h_lo}-{c.h_hi}, saturation >= {c.s_min}, value <= {c.v_max}"
+                f" - saved {BALLS}"
+            )
+            return
         cal = calibration_store.load(out, QVGA) if out.exists() else _defaults()
         if step == "lens":
             k, dist, rms, n = await lens(grab, _ask, out.parent, _show)
@@ -379,12 +412,15 @@ async def _run(step: str, host: str, out: Path) -> None:
 
 def main() -> None:
     ap = argparse.ArgumentParser(description="Calibrate the tank's camera (AI engine LLD §8)")
-    ap.add_argument("step", choices=["lens", "mount", "servos", "study"])
+    ap.add_argument("step", choices=["lens", "mount", "servos", "study", "ball"])
+    ap.add_argument("name", nargs="?", default="", help="ball: the ball's name, e.g. green")
     ap.add_argument("--host", default="192.168.31.167")
     ap.add_argument("--out", type=Path, default=Path("config/calibration.yaml"))
     a = ap.parse_args()
     try:
-        asyncio.run(_run(a.step, a.host, a.out))
+        if a.step == "ball" and not a.name:
+            raise Refused("name the ball: cq-calibrate ball green")
+        asyncio.run(_run(a.step, a.host, a.out, a.name))
     except (Refused, ValueError, ConnectionError, TimeoutError) as e:
         raise SystemExit(f"cq-calibrate {a.step}: {e}") from None
     except CommandRefused as e:
```

- [ ] **Step 3: Run the tests and lint.** Expected: 3 passed; the suite all green; ruff and pyright clean.

- [ ] **Step 4: Commit** — `feat(ai): cq-calibrate ball <name> - a ball's colour into config/balls.yaml (AI engine LLD §16.1 item 5)`

---

### Task 5: On the floor (owner; nothing drives)

Ask "stand or floor?" first. The tank stands still on the floor; only the camera turns, in Step 3. Camera setting B, VGA.

- [ ] **Step 1: VGA checkerboard.**
  - One VGA picture of the board on the laptop screen.
  - Its corners, found by OpenCV, must be within 1 px of the scaled calibration's projection. That is the same check as `cq-calibrate mount`'s marks, done at VGA.
  - If they're off, record the offset and stop. The QVGA → VGA assumption is wrong, and VGA needs its own lens calibration.
- [ ] **Step 2: The ball at 1 m and 2 m** (tape, straight ahead). For each, `cq-record --seconds 8` then `cq-objects`.
  - Expected: one `ball:green` within 5 cm (1 m) and 15 cm (2 m) of the tape plus the lens offset.
- [ ] **Step 3: Memory.**
  - The ball at 1 m. Record 30 s.
  - Meanwhile `cam_set 90 -15` for 12 s, then back to `cam_set 0 -15`.
  - Expected: the ball keeps its `#oid` through the look-away.
- [ ] **Step 4: Things.** A chair and the owner standing ~2 m away. Expected: `chair` and `person` listed.
- [ ] **Step 5: Live.** `cq-objects --live`: boxes on the picture, the plan. Ctrl-C ends it cleanly.
- [ ] **Step 6: New balls.** For each of the child's other balls: `cq-calibrate ball <name>`, then Step 2 at 1 m.
- [ ] **Step 7:** Write the numbers in the ledger; delete the recordings.

### Task 6: Docs as built
- [ ] **Step 1:** AI engine LLD §16.7 "As built": files, numbers, the rulings below, the floor results.
- [ ] **Step 2:** Design-notes §153.
- [ ] **Step 3:** `CLAUDE.md`: add this plan to the plans list; update the order line.
- [ ] **Step 4:** Commit and push. Merge when the owner says so.

## Rulings made while drafting (against LLD §16)
1. **The plug's signature becomes `detect(rgb, horizon_row)`.** The colour finder needs the picture's horizon, and the detector used to run before the pipeline knew that picture's camera. The app now asks `ObjectPipeline.camera_at` first. The test fakes and YOLO26n's adapter take the extra argument (YOLO26n ignores it).
2. **Roundness is judged on the convex hull, threshold 0.66** (LLD: the outline, ≥ 0.55).
   - The outline let squares (0.64) and 2:3 boxes (0.59) pass.
   - The real ball's outline measures only ~0.61, so the outline alone can't tell them apart. Its hull measures 0.73 (p10 0.70).
   - The margin to a square (0.64) is narrow.
   - Score factor 1.3 (the ball scores ~0.95).
3. **Any YOLO-World box on a colour ball is dropped, whatever its word** (LLD: only a "ball" box). On the tank YOLO-World named the green ball "toy".
4. **The overlap limit is 0.7, as Ultralytics uses** (LLD didn't say). At 0.5 we dropped a fourth chair that Ultralytics keeps.
5. **`cq-calibrate ball` looks at the lower half of the picture**, not the computed horizon: the ball is placed ~1 m ahead in the middle of the view.
6. **The camera-setting warning in `cq-objects` (LLD §16.1 item 7) isn't built.** `cq-objects` doesn't read the bridge's camera block. Setting B is written into the docs and into `balls.yaml`'s comment instead.
7. **VGA scaling uses the pixel-centre rule**, `c' = (c + 0.5) × 2 − 0.5`, so a floor point lands exactly on the doubled pixel (tested).
8. **The cabinet is named "bed" / "sofa"** on the tank. Left as it is for now; those words' `min_score` can be raised after the floor checks.

