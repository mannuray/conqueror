# Plan T — `cq-calibrate servos` with any laser line, a few cm off — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `cq-calibrate servos` measures the servo gains itself, the way the session script did on 2026-10-23 (design-notes §130):
- with a laser cross line of any colour, set roughly at lens height;
- sweeping both ways;
- keeping the saved tilt0 and the pan centre.

**Architecture:**
- **`domain/calib_math.py` (pure):** the line finders try redness, greenness and brightness, and take the strongest that passes the peak rules. The servo points take where the line looks at command 0.
- **`app/calibrate.py`:** `servo_tilt` / `servo_pan` ask first, find the line at the centre, sweep down then up (`_sweep`), fit each way, and save the mean gain with offset 0. They print the slack.

**Tech Stack:** Python 3.12 (uv), numpy, pytest, ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §8, "Amended (owner, 2026-10-23, design-notes §130)"; design-notes §130.

**Built from a tested draft:** branch `t-draft`. AI engine 73 tests pass; ruff and pyright are clean.

## Global Constraints
- **Python:** 3.12 via uv, type hints everywhere, ruff (line ≤ 110) and pyright strict clean.
- **The tool's only command to the tank is `cam_set`;** it never drives.
- **Pictures must be 320 × 240** (`qvga()`), as before.
- **tilt0 and the pan centre are never changed by the servo step.** The maps are saved as (gain, 0).
- **Each finder measure passes the existing rules on its own:** `MIN_LINE_SCORE` 20, at most 20 px wide at a quarter of its height, no second peak ≥ 0.5 of it.
- **Hardware:** only the camera moves. Laser: low power, never at eyes (a child is around). Pictures stay local and are deleted after the numbers are recorded.

## Review Focus
1. **A bright white object** (a lamp, a window strip) could pass as a line in the brightness measure. The width and second-peak rules must still reject a broad one. Covered by `test_a_broad_red_object_is_not_a_laser_line`, which now also runs the brightness measure.
2. **A red laser on a white wall** is darker in brightness, so redness must still find it. Covered by the existing red tests (`test_the_laser_line_is_found_to_a_fraction_of_a_pixel`).
3. **The first sweep point is reached from above** (the pre-move to top + 2°, capped at 60° / 90°), and the up sweep skips the lowest point it already sits at. Covered by `test_the_servo_step_sweeps_both_ways_and_reports_the_slack`.
4. **A line far off straight ahead** (22° on the tank) must give pan points that keep it in the picture, inside −90…90. Covered by `test_servo_points_follow_the_line`.
5. **`servos` run before `mount`** must not count the tilt twice. Covered by `test_servos_before_mount_does_not_double_the_tilt`: the servo step keeps tilt0, and the mount step stores `measured − tilt_map offset` (0).

---

### Task 1: Lines of any colour; servo points that follow the line

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/calib_math.py`
- Test: `ai-engine/tests/test_calib_math.py`

**Interfaces:**
- Produces: `laser_row(rgb) -> float | None`, `laser_col(rgb) -> float | None` (any colour); `tilt_points(cal, line_deg: float | None = None) -> list[float]`; `pan_points(cal, line_deg: float = 0.0) -> list[float]`.
- `_peak` becomes `_scored_peak(profile) -> tuple[centre, height] | None`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/ai-engine/tests/test_calib_math.py b/ai-engine/tests/test_calib_math.py
index afbfd03..80f67d7 100644
--- a/ai-engine/tests/test_calib_math.py
+++ b/ai-engine/tests/test_calib_math.py
@@ -126,3 +126,61 @@ def test_servo_points_keep_the_line_inside_the_picture() -> None:
     for p in pans:
         col = c.k[0, 2] - c.k[0, 0] * math.tan(math.radians(p))
         assert 0.1 * 320 <= col <= 0.9 * 320
+
+
+# ---- design-notes §130: a laser of any colour - the owner's is green and burns out to near-white
+def green_burnt_picture(row: float | None = None, col: float | None = None) -> NDArray[np.uint8]:
+    """A dim grey wall with a thin green laser line whose 2-pixel core is burnt out to near-white."""
+    img = np.full((240, 320, 3), 52, dtype=np.uint8)
+    rng = np.random.default_rng(2)
+    img = np.clip(img + rng.normal(0, 4, img.shape), 0, 255).astype(np.uint8)
+    for at, axis in ((row, 0), (col, 1)):
+        if at is None:
+            continue
+        n = img.shape[axis]
+        for i in range(n):
+            d = abs(i - at)
+            px = (175, 215, 190) if d <= 0.5 else ((30, 70, 45) if d <= 1.5 else None)
+            if px is None:
+                continue
+            if axis == 0:
+                img[i, :, :] = px
+            else:
+                img[:, i, :] = px
+    return img
+
+
+def test_a_green_burnt_out_line_is_found() -> None:
+    r = cm.laser_row(green_burnt_picture(row=120.4))
+    assert r is not None and abs(r - 120.4) < 0.8
+    c = cm.laser_col(green_burnt_picture(col=231.0))
+    assert c is not None and abs(c - 231.0) < 0.8
+
+
+def test_a_green_cross_gives_both_lines() -> None:
+    img = green_burnt_picture(row=90.0, col=60.0)
+    r, c = cm.laser_row(img), cm.laser_col(img)
+    assert r is not None and abs(r - 90.0) < 0.8
+    assert c is not None and abs(c - 60.0) < 0.8
+
+
+def test_a_plain_grey_wall_has_no_line() -> None:
+    assert cm.laser_row(green_burnt_picture()) is None
+    assert cm.laser_col(green_burnt_picture()) is None
+
+
+# ---- design-notes §130: servo points follow the line (it need not be at lens height or straight ahead)
+def test_servo_points_follow_the_line() -> None:
+    c = calib(tilt0=0.0)
+    line_tilt = 4.0  # the line looks 4 deg below the centre at command 0 (a few cm off lens height)
+    for t in cm.tilt_points(c, line_tilt):
+        row = c.k[1, 2] + c.k[1, 1] * math.tan(math.radians(line_tilt + t))
+        assert 0.1 * 240 <= row <= 0.9 * 240
+        assert -20.0 <= t <= 60.0
+    line_pan = -21.0  # the vertical line 21 deg to the right at command 0
+    pans = cm.pan_points(c, line_pan)
+    assert len(pans) == 5
+    for p in pans:
+        col = c.k[0, 2] - c.k[0, 0] * math.tan(math.radians(line_pan + p))
+        assert 0.1 * 320 <= col <= 0.9 * 320
+        assert -90.0 <= p <= 90.0
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_calib_math.py`
Expected: `test_a_green_burnt_out_line_is_found` and `test_a_green_cross_gives_both_lines` FAIL (None), and `test_servo_points_follow_the_line` FAILS with a TypeError (`tilt_points` takes 1 argument).

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/ai-engine/src/conqueror_ai/domain/calib_math.py b/ai-engine/src/conqueror_ai/domain/calib_math.py
index d13483e..ff96477 100644
--- a/ai-engine/src/conqueror_ai/domain/calib_math.py
+++ b/ai-engine/src/conqueror_ai/domain/calib_math.py
@@ -2,7 +2,9 @@
 
 - The laser level's horizontal line on a wall at exactly the lens height is the horizon: its row gives the
   camera's real tilt. Its vertical line straight ahead gives the real pan (measured with the camera level).
-- The servo map: commanded angle -> real angle, a straight-line fit.
+- A line of any colour: red, green, or burnt out to near-white (design-notes §130).
+- The servo map: commanded angle -> real angle, a straight-line fit. For the gains only, the line need not be
+  exactly at lens height or straight ahead: only the change between pictures counts (§130).
 - The floor-mark check, and the tilt study (blind zone and view for a few tilts)."""
 
 from __future__ import annotations
@@ -17,7 +19,7 @@ from numpy.typing import NDArray
 from conqueror_ai.domain.camera import Calibration, CameraModel
 from conqueror_ai.domain.types import Pose
 
-MIN_LINE_SCORE = 20.0  # mean red excess on the line's row / column; below it there is no line
+MIN_LINE_SCORE = 20.0  # mean excess (red, green or brightness) on the line's row / column; below it: no line
 MAX_LINE_WIDTH_PX = 20  # wider at a quarter of its height: a red object, not a laser line (final review I5;
 #                         a burnt-out line with its halo is ~15 px at QVGA)
 SECOND_PEAK_RATIO = 0.5  # another peak this strong elsewhere: ambiguous, no answer
@@ -31,10 +33,30 @@ def _redness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
     return np.clip(f[:, :, 0] - np.maximum(f[:, :, 1], f[:, :, 2]), 0.0, None)
 
 
-def _peak(profile: NDArray[np.float64]) -> float | None:
-    """The sub-pixel centre of the one narrow peak, or None: nothing stands out, the peak is wide (a red
-    object), or a second peak nearly as strong is elsewhere. Smoothed first, so a burnt-out line (red
-    only at its fringes) is still one peak."""
+def _greenness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
+    """How much green stands out: G - max(R, B), never below 0."""
+    f = rgb.astype(np.float64)
+    return np.clip(f[:, :, 1] - np.maximum(f[:, :, 0], f[:, :, 2]), 0.0, None)
+
+
+def _brightness(rgb: NDArray[np.uint8]) -> NDArray[np.float64]:
+    """The mean of the three colours: a line burnt out to near-white (§130: the owner's green laser)."""
+    return rgb.astype(np.float64).mean(axis=2)
+
+
+_MEASURES = (_redness, _greenness, _brightness)
+
+
+def _best_line(profiles: list[NDArray[np.float64]]) -> float | None:
+    """The line in whichever measure shows it strongest; each measure must pass _peak's rules on its own."""
+    found = [r for r in (_scored_peak(p) for p in profiles) if r is not None]
+    return max(found, key=lambda r: r[1])[0] if found else None
+
+
+def _scored_peak(profile: NDArray[np.float64]) -> tuple[float, float] | None:
+    """The sub-pixel centre of the one narrow peak and its height, or None: nothing stands out, the peak is
+    wide (a red object), or a second peak nearly as strong is elsewhere. Smoothed first, so a burnt-out line
+    (red only at its fringes) is still one peak."""
     p = np.convolve(profile, np.ones(5) / 5.0, mode="same") - float(np.median(profile))
     i = int(np.argmax(p))
     top = float(p[i])
@@ -53,19 +75,19 @@ def _peak(profile: NDArray[np.float64]) -> float | None:
         return None
     a, b = max(0, lo - 2), min(len(p), hi + 3)
     w = np.clip(p[a:b], 0.0, None)
-    return float((np.arange(a, b) * w).sum() / w.sum())
+    return float((np.arange(a, b) * w).sum() / w.sum()), top
 
 
 def laser_row(rgb: NDArray[np.uint8]) -> float | None:
-    """The row of a horizontal red laser line (mean over the middle half of the columns)."""
+    """The row of a horizontal laser line of any colour (mean over the middle half of the columns)."""
     w = rgb.shape[1]
-    return _peak(_redness(rgb)[:, w // 4 : 3 * w // 4].mean(axis=1))
+    return _best_line([m(rgb)[:, w // 4 : 3 * w // 4].mean(axis=1) for m in _MEASURES])
 
 
 def laser_col(rgb: NDArray[np.uint8]) -> float | None:
-    """The column of a vertical red laser line (mean over the middle half of the rows)."""
+    """The column of a vertical laser line of any colour (mean over the middle half of the rows)."""
     h = rgb.shape[0]
-    return _peak(_redness(rgb)[h // 4 : 3 * h // 4, :].mean(axis=0))
+    return _best_line([m(rgb)[h // 4 : 3 * h // 4, :].mean(axis=0) for m in _MEASURES])
 
 
 def _normalised(cal: Calibration, u: float, v: float) -> tuple[float, float]:
@@ -75,25 +97,31 @@ def _normalised(cal: Calibration, u: float, v: float) -> tuple[float, float]:
     return n
 
 
-def tilt_points(cal: Calibration) -> list[float]:
-    """Commanded tilts for the servo step that keep the horizon line within 10..90 % of the picture's height
-    (assuming about 1 deg per commanded deg) and inside the servo's -20..60 (final review I2)."""
+def tilt_points(cal: Calibration, line_deg: float | None = None) -> list[float]:
+    """Commanded tilts for the servo step that keep the horizontal line within 10..90 % of the picture's
+    height (assuming about 1 deg per commanded deg) and inside the servo's -20..60 (final review I2).
+    `line_deg`: where the line looks at command 0 (`horizon_tilt_deg` of its row, §130); None: a line at
+    lens height (tilt0)."""
     h = cal.size[1]
     fy, cy = float(cal.k[1, 1]), float(cal.k[1, 2])
-    lo = math.degrees(math.atan((INSIDE * h - cy) / fy)) - cal.tilt0_deg
-    hi = math.degrees(math.atan(((1 - INSIDE) * h - cy) / fy)) - cal.tilt0_deg
+    b = cal.tilt0_deg if line_deg is None else line_deg
+    lo = math.degrees(math.atan((INSIDE * h - cy) / fy)) - b
+    hi = math.degrees(math.atan(((1 - INSIDE) * h - cy) / fy)) - b
     lo, hi = max(lo, -20.0), min(hi, 60.0)
     return [round(float(t), 1) for t in np.linspace(lo, hi, SERVO_POINTS)]
 
 
-def pan_points(cal: Calibration) -> list[float]:
-    """Commanded pans (symmetric, with 0) keeping the vertical line within 10..90 % of the picture's width."""
+def pan_points(cal: Calibration, line_deg: float = 0.0) -> list[float]:
+    """Commanded pans keeping the vertical line within 10..90 % of the picture's width and inside -90..90.
+    `line_deg`: where the line looks at command 0 (`pan_from_col_deg` of its column, §130); 0 = straight
+    ahead, and then the points are symmetric with 0 among them."""
     w = cal.size[0]
     fx, cx = float(cal.k[0, 0]), float(cal.k[0, 2])
     right = math.degrees(math.atan((cx - INSIDE * w) / fx))
     left = math.degrees(math.atan(((1 - INSIDE) * w - cx) / fx))
-    m = min(right, left, 90.0)
-    return [round(float(p), 1) for p in np.linspace(-m, m, SERVO_POINTS)]
+    m = min(right, left)
+    lo, hi = max(-m - line_deg, -90.0), min(m - line_deg, 90.0)
+    return [round(float(p), 1) for p in np.linspace(lo, hi, SERVO_POINTS)]
 
 
 def horizon_tilt_deg(cal: Calibration, row: float) -> float:
```

- [ ] **Step 4: Run the tests and the checks**

Run: `cd ai-engine && uv run pytest -q && cd ai-engine && uv run ruff check . && cd ai-engine && uv run ruff format --check . && cd ai-engine && uv run pyright`
Expected: all pass (the tool tests still pass: `calibrate.py` calls `tilt_points(cal)` / `pan_points(cal)` with the defaults); ruff and pyright clean.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/domain/calib_math.py ai-engine/tests/test_calib_math.py
git commit -m "feat(ai): laser lines of any colour; servo points follow the line (AI engine LLD §8, design-notes §130)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: The servo step — ask, sweep both ways, gains only

**Files:**
- Modify: `ai-engine/src/conqueror_ai/app/calibrate.py`
- Test: `ai-engine/tests/test_calibrate_tool.py`

**Interfaces:**
- Consumes: Task 1's `tilt_points(cal, line_deg)`, `pan_points(cal, line_deg)` and any-colour `laser_row` / `laser_col`.
- Produces:
  - `servo_tilt(cal, grab, cam_set, ask) -> Calibration` (**new `ask` parameter**);
  - `servo_pan(cal, grab, cam_set, ask)`;
  - `_sweep(points, pre, measure, what) -> (gain, slack)`.

- [ ] **Step 1: Write the failing tests** (apply this diff; the fake tank gains `line_off` and `slack`):

```diff
diff --git a/ai-engine/tests/test_calibrate_tool.py b/ai-engine/tests/test_calibrate_tool.py
index 76c98c9..e258962 100644
--- a/ai-engine/tests/test_calibrate_tool.py
+++ b/ai-engine/tests/test_calibrate_tool.py
@@ -27,14 +27,22 @@ PAN_GAIN = 1.03
 class FakeTank:
     """Answers cam_set by remembering the angles; makes the picture the camera would see."""
 
-    def __init__(self, scene: str) -> None:
+    def __init__(self, scene: str, line_off: float = 0.0, slack: float = 0.0) -> None:
         self.scene = scene  # "horizon" (laser line at lens height) or "vertical" (line straight ahead)
+        self.line_off = line_off  # deg: the line is a bit off lens height / straight ahead (§130)
+        self.slack = slack  # deg: the servo lands this much higher arriving from above (pan: from the right)
         self.pan = 0.0
         self.tilt = 0.0
+        self.tilt_from = 0.0  # +1 arrived from above, -1 from below
+        self.pan_from = 0.0
         self.sets: list[tuple[float, float]] = []
         self.cal = calib(tilt0=TRUE_TILT0)
 
     async def cam_set(self, pan: float, tilt: float) -> None:
+        if tilt != self.tilt:
+            self.tilt_from = 1.0 if tilt < self.tilt else -1.0
+        if pan != self.pan:
+            self.pan_from = 1.0 if pan < self.pan else -1.0
         self.pan, self.tilt = pan, tilt
         self.sets.append((pan, tilt))
 
@@ -42,8 +50,9 @@ class FakeTank:
         k = self.cal.k
         if self.scene == "horizon":
             real = TRUE_TILT0 + TILT_GAIN * self.tilt + TILT_OFFSET
+            real += self.line_off + self.slack * self.tilt_from
             return red_line_picture(row=float(k[1, 2] + k[1, 1] * math.tan(math.radians(real))))
-        real_pan = PAN_GAIN * self.pan
+        real_pan = PAN_GAIN * self.pan + self.line_off + self.slack * self.pan_from
         return red_line_picture(col=float(k[0, 2] - k[0, 0] * math.tan(math.radians(real_pan))))
 
 
@@ -86,7 +95,7 @@ async def test_the_floor_marks_are_checked_by_clicks() -> None:
 async def test_the_servo_step_fits_the_real_servo() -> None:
     tank = FakeTank("horizon")
     cal = calib(tilt0=TRUE_TILT0 + TILT_OFFSET)  # the mount step's result
-    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set)
+    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set, answers(""))
     gain, offset = cal.tilt_map
     assert math.isclose(gain, TILT_GAIN, abs_tol=0.02) and math.isclose(offset, 0.0, abs_tol=0.3)
     assert tank.sets[-1] == (0.0, 0.0)  # back to the centre
@@ -133,7 +142,7 @@ def test_the_tilt_study_table_has_one_line_per_tilt() -> None:
 async def test_servos_before_mount_does_not_double_the_tilt() -> None:
     tank = FakeTank("horizon")
     cal = calib(tilt0=0.0)
-    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set)  # servos first: the true tilt goes into the fit
+    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set, answers(""))  # servos first: gains only
     cal = await tool.mount_tilt(cal, tank.grab, tank.cam_set, answers(""))  # then mount
     cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), 0.0, 0.0)
     assert math.isclose(cam.real_tilt_deg(), TRUE_TILT0 + TILT_OFFSET, abs_tol=0.3)
@@ -166,5 +175,42 @@ async def test_a_missed_servo_point_is_skipped() -> None:
             return red_line_picture()  # no line in this one
         return await real_grab()
 
-    cal = await tool.servo_tilt(cal, sometimes_blank, tank.cam_set)
+    cal = await tool.servo_tilt(cal, sometimes_blank, tank.cam_set, answers(""))
     assert math.isclose(cal.tilt_map[0], TILT_GAIN, abs_tol=0.02)
+
+
+# ---- design-notes §130: gains only - the line may be a few cm off lens height and not straight ahead
+async def test_the_servo_step_keeps_the_saved_tilt_and_centre() -> None:
+    tank = FakeTank("horizon", line_off=-1.2)  # the line ~2 cm below lens height at 1 m
+    cal = calib(tilt0=-0.15)
+    cal = await tool.servo_tilt(cal, tank.grab, tank.cam_set, answers(""))
+    assert cal.tilt0_deg == -0.15  # kept (the tape marks' tilt)
+    assert math.isclose(cal.tilt_map[0], TILT_GAIN, abs_tol=0.02) and cal.tilt_map[1] == 0.0
+    tank.scene, tank.line_off = "vertical", -21.0  # the vertical line well to the right
+    cal = await tool.servo_pan(cal, tank.grab, tank.cam_set, answers(""))
+    assert math.isclose(cal.pan_map[0], PAN_GAIN, abs_tol=0.02)
+    assert cal.pan_map[1] == 0.0  # the Uno's camera centre is kept
+    assert tank.sets[-1] == (0.0, 0.0)
+
+
+async def test_the_servo_step_sweeps_both_ways_and_reports_the_slack(capsys: object) -> None:
+    tank = FakeTank("horizon", slack=0.8)
+    cal = await tool.servo_tilt(calib(tilt0=0.0), tank.grab, tank.cam_set, answers(""))
+    assert math.isclose(cal.tilt_map[0], TILT_GAIN, abs_tol=0.02)  # the slack averages out
+    tilts = [t for _, t in tank.sets]
+    assert tilts.index(min(tilts)) < len(tilts) - 2  # down, then back up
+    assert tilts.index(max(tilts)) < len(tilts) - 2
+    out = capsys.readouterr().out  # type: ignore[attr-defined]
+    assert "slack" in out and "0.8" in out
+
+
+async def test_the_servo_step_asks_for_the_horizontal_line_first() -> None:
+    tank = FakeTank("horizon")
+    asked: list[str] = []
+
+    async def ask(prompt: str) -> str:
+        asked.append(prompt)
+        return ""
+
+    await tool.servo_tilt(calib(tilt0=0.0), tank.grab, tank.cam_set, ask)
+    assert len(asked) == 1 and "roughly at the lens height" in asked[0] and tank.sets[1:] != []
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_calibrate_tool.py`
Expected: TypeError `servo_tilt() takes 3 positional arguments but 4 were given` in the servo tests.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/ai-engine/src/conqueror_ai/app/calibrate.py b/ai-engine/src/conqueror_ai/app/calibrate.py
index 861e63e..73d8280 100644
--- a/ai-engine/src/conqueror_ai/app/calibrate.py
+++ b/ai-engine/src/conqueror_ai/app/calibrate.py
@@ -2,7 +2,8 @@
 
   lens    the checkerboard on the laptop screen, ~20 pictures -> focal length, centre, distortion
   mount   the measured mount, the laser level's horizon line -> the real tilt, then two tape marks to check
-  servos  commanded -> real tilt (the horizon line) and pan (the level's vertical line, camera level)
+  servos  commanded -> real tilt and pan gains: a laser cross line of any colour, roughly at the lens height
+          (design-notes §130); tilt0 and the pan centre are kept
   study   the blind zone and the view at a few tilts, for the owner's driving-tilt decision
 
 Each step saves config/calibration.yaml. The tool's only command to the tank is cam_set (bridge LLD R22): it
@@ -140,48 +141,80 @@ async def floor_marks(cal: Calibration, grab: Grab, click: Click, ask: Ask, cam_
     return errors
 
 
-async def servo_tilt(cal: Calibration, grab: Grab, cam_set: CamSet) -> Calibration:
-    """Commanded tilt -> real tilt, from the horizon line at points chosen to keep it inside the picture.
-    The fit's offset becomes tilt0 (the real tilt at command 0), so tilt0 and the map never count it twice
-    (final review I3)."""
-    done: list[float] = []
-    real: list[float] = []
-    for t in cm.tilt_points(cal):
+async def _sweep(
+    points: list[float], pre: float, measure: Callable[[float], Awaitable[float | None]], what: str
+) -> tuple[float, float]:
+    """Both ways across `points`: down from `pre` (every point reached from above / the right), then back up.
+    Each way gets its own straight-line fit, so the servo's slack cancels out of the gain (design-notes §130).
+    Returns (gain, slack): the mean gain, and half the gap between the two ways' offsets (deg)."""
+    fits: list[tuple[float, float]] = []
+    down = sorted(points, reverse=True)
+    up = sorted(points)[1:]  # the lowest is already reached on the way down
+    for i, way in enumerate((down, up)):
+        if i == 0:
+            await measure(pre)  # only to reach the first point from above (its answer is not used)
+        done: list[float] = []
+        real: list[float] = []
+        for x in way:
+            r = await measure(x)
+            if r is None:
+                print(f"  no line at {what} {x:+.1f} - skipped")
+                continue
+            done.append(x)
+            real.append(r)
+        if len(done) < MIN_SERVO_POINTS:
+            raise Refused(f"too few {what}s saw the line - is it in the picture and bright enough?")
+        fits.append(cm.fit_linear(done, real))
+    (g_down, o_down), (g_up, o_up) = fits
+    return (g_down + g_up) / 2.0, (o_down - o_up) / 2.0
+
+
+async def servo_tilt(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
+    """Commanded tilt -> real tilt: the gain only (design-notes §130). The line may be a few cm off the lens
+    height, because only the change between pictures counts; the saved tilt0 (from the mount step or the tape
+    marks) is kept, and the map's offset stays 0, so they never count twice (final review I3)."""
+    await ask(
+        "Laser level: the horizontal line on the wall ahead, roughly at the lens height (+-5 cm). Enter: "
+    )
+    await cam_set(0.0, 0.0)
+    row0 = cm.laser_row(qvga(await grab()))
+    if row0 is None:
+        raise Refused("no horizontal line at the centre - is it in the picture and bright enough?")
+    points = cm.tilt_points(cal, cm.horizon_tilt_deg(cal, row0))
+
+    async def measure(t: float) -> float | None:
         await cam_set(0.0, t)
         row = cm.laser_row(qvga(await grab()))
-        if row is None:
-            print(f"  no horizon line at tilt {t:+.1f} - skipped")
-            continue
-        done.append(t)
-        real.append(cm.horizon_tilt_deg(cal, row))
+        return None if row is None else cm.horizon_tilt_deg(cal, row)
+
+    gain, slack = await _sweep(points, min(max(points) + 2.0, 60.0), measure, "tilt")
     await cam_set(0.0, 0.0)
-    if len(done) < MIN_SERVO_POINTS:
-        raise Refused(
-            "too few tilts saw the horizon line - keep the laser line at the lens height and try again"
-        )
-    gain, offset = cm.fit_linear(done, real)
-    return replace(cal, tilt0_deg=offset, tilt_map=(gain, 0.0))
+    print(f"  tilt: {gain:.3f} real deg per commanded deg; slack +-{abs(slack):.1f} deg")
+    return replace(cal, tilt_map=(gain, 0.0))
 
 
 async def servo_pan(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
-    """Commanded pan -> real pan, from the laser level's vertical line straight ahead, the camera level."""
+    """Commanded pan -> real pan: the gain only (design-notes §130), from the laser level's vertical line
+    anywhere in the picture, the camera level. The pan centre stays the Uno's saved camera centre
+    (offset 0)."""
     g, o = cal.tilt_map
     level = max(-20.0, min(60.0, (-cal.tilt0_deg - o) / g))
-    await ask("Laser level: a vertical line straight ahead of the tank, on the wall. Enter: ")
-    done: list[float] = []
-    real: list[float] = []
-    for p in cm.pan_points(cal):
+    await ask("Laser level: the vertical line on the wall ahead, roughly straight ahead. Enter: ")
+    await cam_set(0.0, level)
+    col0 = cm.laser_col(qvga(await grab()))
+    if col0 is None:
+        raise Refused("no vertical line at the centre - is it in the picture and bright enough?")
+    points = cm.pan_points(cal, cm.pan_from_col_deg(cal, col0))
+
+    async def measure(p: float) -> float | None:
         await cam_set(p, level)
         col = cm.laser_col(qvga(await grab()))
-        if col is None:
-            print(f"  no vertical line at pan {p:+.1f} - skipped")
-            continue
-        done.append(p)
-        real.append(cm.pan_from_col_deg(cal, col))
+        return None if col is None else cm.pan_from_col_deg(cal, col)
+
+    gain, slack = await _sweep(points, min(max(points) + 2.0, 90.0), measure, "pan")
     await cam_set(0.0, 0.0)
-    if len(done) < MIN_SERVO_POINTS:
-        raise Refused("too few pans saw the vertical line - is it straight ahead and bright enough?")
-    return replace(cal, pan_map=cm.fit_linear(done, real))
+    print(f"  pan: {gain:.3f} real deg per commanded deg; slack +-{abs(slack):.1f} deg")
+    return replace(cal, pan_map=(gain, 0.0))
 
 
 def study_table(rows: list[cm.TiltRow]) -> str:
@@ -294,12 +327,9 @@ async def _run(step: str, host: str, out: Path) -> None:
                     print("not saved")
                     return
         elif step == "servos":
-            cal = await servo_tilt(cal, grab, cam_set)
+            cal = await servo_tilt(cal, grab, cam_set, _ask)
             cal = await servo_pan(cal, grab, cam_set, _ask)
-            print(
-                f"servos: tilt {cal.tilt_map[0]:.3f} x + {cal.tilt_map[1]:+.2f}, "
-                f"pan {cal.pan_map[0]:.3f} x + {cal.pan_map[1]:+.2f}"
-            )
+            print(f"servos: tilt {cal.tilt_map[0]:.3f} x, pan {cal.pan_map[0]:.3f} x (tilt0, centre kept)")
         elif step == "study":
             print(study_table(cm.tilt_study(cal, STUDY_TILTS)))
             return
```

- [ ] **Step 4: Run all tests and the checks**

Run: `cd ai-engine && uv run pytest -q && cd ai-engine && uv run ruff check . && cd ai-engine && uv run ruff format --check . && cd ai-engine && uv run pyright`
Expected: `73 passed`; ruff and pyright clean.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/app/calibrate.py ai-engine/tests/test_calibrate_tool.py
git commit -m "feat(ai): cq-calibrate servos - gains only, both ways, the slack printed (design-notes §130)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: On the tank — run `cq-calibrate servos` (owner present, laser set up)

Only the camera moves. Ask the owner "stand or floor?" first, and say that only the camera moves.

- [ ] **Step 1:** Set the camera to QVGA: `{"t":"camera_set","size":"QVGA"}` (upper case).
- [ ] **Step 2:** Run `cd ai-engine && uv run cq-calibrate servos`. At each prompt, the owner places the laser line and presses Enter.
  - Expected: tilt gain about 0.91 and pan gain about 0.94, each within ±0.02 of §130; slack about ±0.8° (tilt) and ±0.4° (pan); `saved config/calibration.yaml`.
  - The tool keeps tilt0 −0.15 and the offsets at 0.
- [ ] **Step 3:** Set the camera back to VGA. Compare the saved file with main's (`git diff ai-engine/config/calibration.yaml`).
  - Keep the tool's values if they are within ±0.02 of §130, and write them down.
  - Otherwise, stop and discuss with the owner.
- [ ] **Step 4: Commit** the calibration if it changed:

```bash
git add ai-engine/config/calibration.yaml
git commit -m "chore(ai): servo gains from cq-calibrate servos

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: Docs as built

**Files:**
- `docs/lld/ai-engine-lld.md` (the §8 amendment marked as built; §13 the tool's run);
- `docs/design-notes.md` (the run's results, with how they compare to §130);
- `CLAUDE.md` (plan list: `2026-10-23-plan-t-servo-gains.md`).

- [ ] **Step 1: Write them.**
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan T servo gains tool - as built

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
