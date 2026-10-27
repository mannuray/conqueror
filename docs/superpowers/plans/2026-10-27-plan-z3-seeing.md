# Plan Z3 — Seeing: depth, the camera's real angle, heights — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every picture the map uses gets a depth picture, the camera's real tilt (from the depth floor), real distances, and a height for every point (floor / low / high / above), shown in the live view; every camera distance (the ball's too) uses the corrected tilt.

**Architecture:** A CoreML adapter runs Depth Anything V2 Small beside the detector. Pure domain parts (`CameraAngle`, `DepthScaler`, `HeightPoints`, glued by `Seeing`) turn its relative inverse depth into a tilt fix, metric ranges and height-classed points. The tilt fix enters `CameraModel.tilt_fix_deg`, which `FrameGate` applies to every picture's camera, so the object tracker's floor trick uses it too.

**Tech Stack:** Python 3.12 (uv), numpy, OpenCV, coremltools (CoreML, Apple neural engine), rerun-sdk 0.39, pytest, ruff, pyright strict.

**Spec:** `docs/superpowers/specs/2026-10-27-09-explore-without-bumping-design.md` (§3, §5, §6.1-6.2) and `docs/lld/ai-engine-lld.md` §18.0-§18.4, §18.9-§18.10 (Z3 parts). Decision record: `docs/design-notes.md` §155, §159.

## Global Constraints
- Python 3.12 via uv; type hints everywhere; `uv run ruff check .`, `uv run ruff format --check .`, `uv run pyright` clean (strict); asyncio never blocked (model calls in `asyncio.to_thread`).
- **No new installs.** The model is `tools/measure/models/da2s/DepthAnythingV2SmallF16.mlpackage`, copied to the git-ignored `ai-engine/models/`. Input `image` 518 × 392 RGB, output `depth` 518 × 392 float16 — **relative inverse depth** (design-notes §109).
- Heights (owner, design-notes §159): floor `< 0.02` m; low `0.02 ≤ z < 0.10`; high `0.10 ≤ z ≤ 0.20`; above `> 0.20`; points with `z < −0.05` dropped. `clear_m` (0.20) may only be made larger.
- Ranges: obstacles (low / high) to `mark_max_m` 2.0 m; floor and above to `free_max_m` 3.0 m.
- Camera angle (design-notes §155, LLD §18.4): depth tilt = horizon of the floor's inverse-depth line − `tilt_offset_deg` 4.1° (+ = up); fix = median over the last 1 s of (depth tilt − the servo's real tilt); **trusted when |fix| ≤ 3.0°**, else fix 0 and untrusted; a picture's estimate counts only when the line has ≥ 20 rows, R² ≥ 0.98, slope < 0.
- Every 4th pixel in both directions for `HeightPoints` (160 × 120 at VGA).
- Frames as in the LLD §13: room x forward, y left, z up; image x right, y down; `real_tilt_deg` + = up.
- Pictures and recordings show the home: keep them local (`recordings/` is git-ignored), delete after the numbers are written down.
- Floor checks only with the owner present and STOP in hand; say what moves and give numbered steps before each.
- Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; branch `plan-z3`.

## Review Focus
1. **The floor-depth cache ignores the tilt fix:** `expected_floor_depth()` is cached per `(id(cal), pan, tilt)`; a camera with a different `tilt_fix_deg` must not get the old depths (Task 2 test).
2. **No floor in view** (the tank close to a wall or sofa): no angle estimate, no scale, no marks — the run goes on with the old fix and nothing marked, never a crash (Tasks 3, 4, 6 tests).
3. **Bad depth values** (NaN, inf, zero, negative inverse depth): dropped, never a point at a silly height (Tasks 4, 5 tests).
4. **The model file missing:** `cq-objects` and `cq-drive` say so plainly and run without depth (Task 7 test).
5. **A picture of another size than the calibration** (QVGA recording vs VGA calibration): the depth is resized to the picture, and the existing "picture size" skip still applies (Task 7 test).

---

### Task 1: `config/map.yaml` and its loader

**Files:**
- Create: `ai-engine/config/map.yaml`, `ai-engine/src/conqueror_ai/adapters/map_config.py`, `ai-engine/src/conqueror_ai/domain/depth.py` (the settings dataclasses; later tasks add to it)
- Test: `ai-engine/tests/test_map_config.py`

**Interfaces:**
- Produces: `@dataclass(frozen=True) Heights(floor_m=0.02, low_top_m=0.10, clear_m=0.20, below_m=-0.05)`, `Ranges(mark_max_m=2.0, free_max_m=3.0)`, `DepthSettings(every_n=1, tilt_offset_deg=4.1, trust_deg=3.0)`, `MapConfig(heights: Heights, ranges: Ranges, depth: DepthSettings)` in `domain/depth.py` (pure, used by the domain); `load_map_config(path: Path) -> MapConfig` in `adapters/map_config.py`; `MAP_CONFIG = ENGINE / "config" / "map.yaml"` in `app/objects.py`.

- [ ] **Step 1: Write the failing tests**
```python
def test_the_defaults_match_the_file(): assert load_map_config(MAP_CONFIG) == MapConfig(Heights(), Ranges(), DepthSettings())
def test_a_lower_clear_height_is_refused(tmp_path):  # heights.clear_m: 0.15 -> ValueError("... may only be made more careful - clear_m")
def test_an_unknown_key_is_refused(tmp_path):        # heights.foo: 1 -> ValueError naming "foo"
def test_a_missing_section_keeps_its_defaults(tmp_path):  # only depth: {every_n: 2} -> every_n 2, the rest default
```
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_map_config.py -q` → FAIL (no module).
- [ ] **Step 3:** Write `config/map.yaml` with the three sections (`heights`, `ranges`, `depth`, LLD §18.9 values, a comment citing design-notes §159) and `load_map_config` (yaml → dataclasses by field name; unknown keys and `clear_m < 0.20` refused, the message style of `drive_config.py`).
- [ ] **Step 4:** Tests pass.
- [ ] **Step 5:** Commit `feat(ai): config/map.yaml - heights, ranges, depth settings (LLD §18.9)`.

### Task 2: the tilt fix in `CameraModel` and `FrameGate`

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/camera.py` (`CameraModel`, `real_tilt_deg`, the `_DEPTHS` key), `ai-engine/src/conqueror_ai/domain/objects.py` (`FrameGate`, `ObjectPipeline`)
- Test: `ai-engine/tests/test_camera.py`, `ai-engine/tests/test_objects.py`

**Interfaces:**
- Produces: `CameraModel.tilt_fix_deg: float = 0.0` (last field); `real_tilt_deg() = tilt0 + g·tilt + o + tilt_fix_deg`; `FrameGate.tilt_fix_deg: float` (default 0.0) used in `camera_for`; `ObjectPipeline.set_tilt_fix(deg: float) -> None`.

- [ ] **Step 1: Write the failing tests**
```python
def test_the_tilt_fix_adds_to_the_real_tilt():  # CameraModel(cal, tilt_deg=-15, tilt_fix_deg=2.0).real_tilt_deg() == base + 2.0
def test_the_floor_depth_cache_tells_tilt_fixes_apart():
    a = CameraModel(cal, tilt_deg=-15.0).expected_floor_depth()
    b = CameraModel(cal, tilt_deg=-15.0, tilt_fix_deg=2.0).expected_floor_depth()
    assert not np.allclose(a[np.isfinite(a) & np.isfinite(b)], b[np.isfinite(a) & np.isfinite(b)])
def test_every_picture_gets_the_pipelines_tilt_fix():  # p.set_tilt_fix(1.5); cam = p.camera_at(t) -> cam.tilt_fix_deg == 1.5
def test_the_floor_trick_moves_with_the_tilt_fix():   # pixel_to_floor of a fixed pixel lands nearer with a fix of -1 deg (more down)
```
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Add the field; add `tilt_fix_deg` to the `_DEPTHS` key; `FrameGate.camera_for` builds `CameraModel(self.cal, tilt_fix_deg=self.tilt_fix_deg).for_frame(...)` (or `replace` after); `ObjectPipeline.set_tilt_fix` sets `self.gate.tilt_fix_deg`.
- [ ] **Step 4:** `uv run pytest tests/test_camera.py tests/test_objects.py -q` → PASS (all old tests too).
- [ ] **Step 5:** Commit `feat(ai): CameraModel.tilt_fix_deg, applied to every picture by FrameGate (LLD §18.4)`.

### Task 3: `CameraAngle`

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/depth.py` (Task 1 made it)
- Test: `ai-engine/tests/test_depth.py`

**Interfaces:**
- Consumes: `CameraModel` (Task 2), `DepthSettings` (Task 1).
- Produces: `@dataclass(frozen=True) AngleFix(tilt_fix_deg: float, trusted: bool, depth_tilt_deg: float | None, n: int)`; `CameraAngle(settings: DepthSettings, window_s: float = 1.0)` with `update(cam: CameraModel, inv: FloatArray, t_us: int) -> AngleFix` and `fix() -> AngleFix` (starts `AngleFix(0.0, True, None, 0)`); helper `depth_tilt_deg(cam, inv, offset_deg) -> float | None`.

`depth_tilt_deg` (the §155 method, decided here):
1. rows: those where the centre column of `cam.floor_mask()` is true; columns: the middle half of the picture; per row the median of `inv` (finite, > 0 only);
2. fit `inv = p·row + q` (least squares), drop rows with residual > 3σ once, refit; reject when < 20 rows, R² < 0.98 or `p ≥ 0` (inverse depth must grow toward the bottom);
3. horizon row `row0 = −q / p`; normalised `yn` from `cam.undistort(0.0, (row0 − cy) / fy)` (pixel centre column); `None` if that fails;
4. pitch (+ up) `= degrees(atan(yn)) − offset_deg`. A horizon above the centre (`row0 < cy`, so `yn < 0`) means the camera points down; the offset is taken off because the depth reads 4.1° less down than the truth (§155).

`update`: compute `depth_tilt_deg(cam_with_fix_0, inv, offset)`; if not None, push `(t_us, depth_tilt − cam_with_fix_0.real_tilt_deg())`; drop entries older than `window_s`; fix = median; trusted = |fix| ≤ `trust_deg`; when untrusted the returned `tilt_fix_deg` is 0.0; with no entries the last fix stays.

- [ ] **Step 1: Write the failing tests** (synthetic inverse depth: `inv = 1.7 / true_cam.expected_floor_depth() + 0.05`, `inf` → 0, where `true_cam` is the servo camera with `tilt_fix_deg = true_fix`; offset 0 in `DepthSettings` for these):
```python
def test_the_depth_floor_finds_a_known_tilt_error():        # true_fix +2.0 -> fix within 0.3 deg, trusted
def test_the_offset_is_taken_off():                          # offset 4.1, inv from a camera 4.1 deg further up -> fix ~0
def test_a_fix_over_3_deg_is_not_trusted():                  # true_fix 4.0 -> trusted False, tilt_fix_deg 0.0, depth_tilt_deg set
def test_the_median_of_the_last_second_rules():              # 4 pictures at +2, 1 at +10 within 1 s -> ~2; entries older than 1 s dropped
def test_no_floor_in_view_keeps_the_last_fix():              # inv all equal (a wall) -> no estimate; fix unchanged
def test_bad_values_are_ignored():                           # NaN / inf / negative sprinkled in inv -> same fix within 0.3
```
- [ ] **Step 2:** Run → FAIL.
- [ ] **Step 3:** Implement as above.
- [ ] **Step 4:** PASS.
- [ ] **Step 5:** Commit `feat(ai): CameraAngle - the camera's real tilt from the depth floor (LLD §18.4, design-notes §155)`.

### Task 4: `DepthScaler`

**Files:** Modify `domain/depth.py`; Test `tests/test_depth.py`.

**Interfaces:**
- Produces: `@dataclass(frozen=True) ScaledDepth(range_m: FloatArray, a: float, c: float, n: int)`; `DepthScaler(min_pixels: int = 500)` with `fit(cam: CameraModel, inv: FloatArray) -> ScaledDepth | None`.

- [ ] **Step 1: Write the failing tests**
```python
def test_the_floor_scale_is_recovered():          # inv = (1/expected - 0.05) / 1.7 on the floor -> a ~1.7, c ~0.05; range_m == expected on the floor (1 %)
def test_too_little_floor_gives_none():           # floor pixels masked to 400 -> None
def test_a_wrong_way_fit_gives_none():            # inv decreasing toward the bottom -> a <= 0 -> None
def test_ranges_where_the_fit_goes_negative_are_inf_not_negative():
def test_bad_inverse_depth_values_are_left_out(): # NaN / inf / <= 0 never used in the fit, give inf in range_m
```
- [ ] **Step 2:** FAIL. **Step 3:** least squares `1/expected = a·inv + c` on `cam.floor_mask()` pixels with finite `inv > 0`; one 3σ trim and refit; `None` if fewer than `min_pixels` or `a ≤ 0`; `range_m = 1/(a·inv + c)` where the denominator > 0 and `inv` is finite, else `inf`. **Step 4:** PASS. **Step 5:** Commit `feat(ai): DepthScaler - depth in metres fitted on the floor (LLD §18.4)`.

### Task 5: `HeightPoints` and `Marks`

**Files:** Modify `domain/depth.py`; Test `tests/test_depth.py`.

**Interfaces:**
- Produces: `@dataclass(frozen=True) Marks(floor_xy: FloatArray, low_xy: FloatArray, high_xy: FloatArray, above_xy: FloatArray)` (each `(n, 2)` room metres); `HeightPoints(heights: Heights, ranges: Ranges, stride: int = 4)` with `classify(cam: CameraModel, range_m: FloatArray) -> Marks`.

Point = `cam.position() + unit(ray) × range` for every `stride`-th pixel (rays from the same vectorised undistortion as `expected_floor_depth`; factor it into a cached `CameraModel.unit_rays() -> FloatArray (h, w, 3)` in the room frame, cached like `_DEPTHS` with the same key). Distance limits use the horizontal distance from the lens.

- [ ] **Step 1: Write the failing tests** (helper `range_for_height(cam, u, v, z)` = the range along pixel (u, v)'s ray where it reaches height z):
```python
def test_floor_pixels_are_floor():                       # range = expected floor depth -> all in floor_xy, near their floor_to_pixel points
def test_a_box_gives_low_and_high_points():              # a patch at z 0.05 -> low; at z 0.15 -> high
def test_an_18_cm_board_is_high_and_a_30_cm_one_is_above()
def test_obstacles_beyond_2_m_are_not_marked_floor_to_3_m():
def test_points_under_the_floor_and_bad_ranges_are_dropped():  # z -0.10, inf, NaN -> nowhere
def test_the_owners_clear_height_is_used():             # Heights(clear_m=0.25): z 0.22 -> high
```
- [ ] **Step 2:** FAIL. **Step 3:** implement (numpy, no loops over pixels). **Step 4:** PASS. **Step 5:** Commit `feat(ai): HeightPoints - every point's height: floor, low, high, above (LLD §18.4)`.

### Task 6: `Seeing` — one picture, end to end

**Files:** Modify `domain/depth.py`; Test `tests/test_depth.py`.

**Interfaces:**
- Consumes: Tasks 2-5.
- Produces: `@dataclass(frozen=True) Seen(fix: AngleFix, cam: CameraModel, scaled: ScaledDepth | None, marks: Marks | None)`; `Seeing(cfg: MapConfig)` with `see(cam: CameraModel, inv: FloatArray, t_us: int) -> Seen` — updates `CameraAngle`, re-makes the camera with `replace(cam, tilt_fix_deg=fix.tilt_fix_deg)`, scales, classifies; `scaled`/`marks` are None when the scale fails.

- [ ] **Step 1: Write the failing tests**
```python
def test_a_synthetic_scene_comes_out_right():   # true fix +1.5, a 10 cm box patch, offset 0: fix ~1.5, the box's low points within 5 cm of where it is
def test_a_wall_close_up_gives_no_marks_and_keeps_the_fix():
```
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS; then `uv run pytest -q` (whole suite) passes. **Step 5:** Commit `feat(ai): Seeing - depth to the tilt fix, metres and heights in one call (LLD §18.3)`.

### Task 7: `DepthCoreml`, and depth in `cq-objects` and `cq-drive`

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/depth_coreml.py`
- Modify: `app/objects.py` (`DEPTH_MODEL`, `MAP_CONFIG`, `run_replay`, `run_live`, `report`, `main`), `adapters/rerun_view.py` (`depth`, `marks`), `app/drive.py` (the loop: depth → `Seeing` → `p.set_tilt_fix`)
- Test: `ai-engine/tests/test_depth_coreml.py`, `ai-engine/tests/test_objects_app.py`

**Interfaces:**
- Produces: `DepthCoreml(path: Path)` with `infer(rgb: NDArray[uint8]) -> FloatArray` (the picture's size, float32); `DepthFn = Callable[[NDArray[uint8]], FloatArray]`; `DEPTH_MODEL = ENGINE / "models" / "DepthAnythingV2SmallF16.mlpackage"`; `run_replay(folder, cal, det, floor_classes, depth: DepthFn | None = None, cfg: MapConfig | None = None) -> ReplayResult` with new fields `angle_fixes: list[float]`, `trusted: int`, `estimates: int`, `seconds: float`; `RerunView.depth(f: Frame, range_m: FloatArray) -> None` (`camera/depth`, `rr.DepthImage(range_m, meter=1.0)`, inf → 0) and `RerunView.marks(t_us: int, marks: Marks) -> None` (`plan/floor` grey, `plan/low` red, `plan/high` orange, `plan/above` light blue, Points2D, radius 0.02).

- [ ] **Step 0 (setup):** `mkdir -p ai-engine/models && cp -R tools/measure/models/da2s/DepthAnythingV2SmallF16.mlpackage ai-engine/models/` (git-ignored; nothing downloaded).
- [ ] **Step 1: Write the failing tests**
```python
@pytest.mark.skipif(not DEPTH_MODEL.exists(), reason="no model file (copy it from tools/measure/models/da2s)")
def test_the_model_gives_inverse_depth_at_the_pictures_size():  # a 640x480 test picture -> (480, 640) float32, finite, larger at the bottom (the floor) than the top
def test_replay_without_depth_reports_no_angle():                # report has no "camera angle" line
def test_replay_with_a_fake_depth_reports_the_angle_and_rate():  # fake depth = flat array -> "camera angle: no estimate"; "pictures/s" printed
def test_a_missing_depth_model_runs_without_depth(tmp_path):    # main with --depth-model tmp_path/none -> prints "no depth model ... running without depth", exit 0
def test_a_picture_of_another_size_still_skips():                # QVGA frame with the VGA calibration -> "picture size", depth not called
```
- [ ] **Step 2:** FAIL.
- [ ] **Step 3:** Implement:
  - `DepthCoreml`: load like `YoloCoreml` (`compute_units=ALL`); `PIL.Image.fromarray(rgb).resize((518, 392), BILINEAR)` → `predict({"image": img})["depth"]` → `np.asarray(..., float32)` → `cv2.resize(..., (w, h), INTER_LINEAR)`.
  - `run_replay` / `run_live`: when depth is on, run `depth` and `det.detect` in threads together (`asyncio.gather` of two `to_thread` calls in live; one after the other in replay); a used picture goes to `Seeing.see`, then `p.set_tilt_fix(seen.fix.tilt_fix_deg)`; the view logs `depth` and `marks`; `report` adds `camera angle: fix {median:+.1f} deg, trusted {trusted}/{estimates}` (or `no estimate`) and `{pictures/s} pictures/s` (used pictures over the run's seconds).
  - `main`: `--depth-model` (default `DEPTH_MODEL`), `--no-depth`; a missing file prints the plain message and runs without depth.
  - `cq-drive goto`: the same depth + `Seeing` + `set_tilt_fix` per used picture (no marks used yet); `--no-depth` too.
- [ ] **Step 4:** `uv run pytest -q` → all PASS; ruff, format, pyright clean.
- [ ] **Step 5:** Commit `feat(ai): depth in cq-objects and cq-drive - the tilt fix live, depth and heights in the view (LLD §18.3)`.

### Task 8: Measure on the laptop and the floor (owner present)

No code unless a check fails (then: systematic debugging, a test that fails first, a fix, and a ledger ruling).

- [ ] **Step 1 (laptop, no tank): the rate.** Record 20 s standing (`uv run cq-record recordings/z3-rate`, the tank still, camera at the driving tilt), then `uv run cq-objects recordings/z3-rate` → write down pictures/s with depth and with `--no-depth`. **Pass:** ≥ 5 pictures/s with depth (LLD §18.4); otherwise set `depth.every_n: 2` and measure again.
- [ ] **Step 2 (floor): the camera angle, two places.** Before it: say what moves (only the camera tilts to −15°; the tank stays still). Ball's centre taped at **1.0 m** and **2.0 m** straight ahead of the lens; `cq-objects --live`, 10 s each, then the same recorded and replayed with `--no-depth`. Write down the ball's distance with and without the fix and the reported fix. **Pass (spec 9 §6.1):** with the fix, the ball within **10 cm at 1 m** (1° ≈ 10 cm there) and **25 cm at 2 m**, in both places.
- [ ] **Step 3 (floor): heights.** A box ~10 cm tall at ~70 cm, a board across two boxes at ~18 cm at ~1 m, a chair (seat > 20 cm) at ~1.5 m. Live view: the box red (low) with an orange top only if over 10 cm, the board orange (high), the seat light blue (above), the floor grey. **Pass (spec 9 §6.2, for this plan):** none of the three shown as floor within 1 m.
- [ ] **Step 4 (owner): the tank's highest point.** The owner measures it with the camera level. Over 17 cm → `heights.clear_m` = top + 0.03 in `config/map.yaml` (design-notes §159).
- [ ] **Step 5:** Delete the recordings; write the numbers (how each was derived) into design-notes §160; commit `docs: Plan Z3 floor checks (design-notes §160)`.

### Task 9: Docs as built
- [ ] **Step 1:** AI engine LLD §18.11 "Z3 as built": files, any change from §18.4 with the reason, the numbers from Task 8.
- [ ] **Step 2:** `CLAUDE.md`: Plan Z3 in the plans list and the order line (next: Plan Z4, the room map).
- [ ] **Step 3:** Commit and push; merge only when the owner says so.
