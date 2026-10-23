# Plan V — Phase 4: the detector and the object tracker — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The AI engine finds things in each picture (YOLO26n on the Neural Engine), works out where each stands in the room (the floor trick), and keeps a list that remembers things out of view. `cq-objects` shows it live and prints it from a recording.

**Architecture:**
- **`domain/objects.py` (pure):** `CameraAngles` + `FrameGate` (the camera at the capture time, or a skip reason), `ObjectLocator` (floor trick, near things), `ObjectTracker` (Kalman per thing, matched in room positions, Seen / Remembered / forgotten), and `ObjectPipeline`.
- **`adapters/yolo_coreml.py`:** the CoreML model with letterbox and box mapping. The model is made by `tools/measure/export_yolo.py`.
- **`app/objects.py`** (`cq-objects`): replay (deterministic) and live (the detector in a worker thread, latest picture wins), plus two `RerunView` methods.

**Tech Stack:** Python 3.12 (uv), numpy, OpenCV, coremltools (run time only), Pillow, Rerun 0.39, pytest, ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §15 (approved 2026-10-23); design-notes §134; Spec 7 §4, §7, §9 phase 4.

**Built from a tested draft:** branch `v-draft`. AI engine 134 tests pass (the model test runs when the model file exists); ruff and pyright are clean. The real model was checked on Ultralytics' bus test picture: 4 people + the bus, boxes in picture pixels.

## Global Constraints
- **Python:** 3.12 via uv, type hints everywhere, ruff (line ≤ 110) and pyright strict clean. Domain code is pure.
- **The engine never sends motion** (Spec 7 §1).
- **The model file** `ai-engine/models/yolo26n-320.mlpackage` is never committed (`models/` git-ignored; ~5 MB, AGPL). It's made by `cd tools/measure && uv run python export_yolo.py`.
- **Constants (LLD §15):**
  - `CONF` 0.35, `GATE_DPS` 45, `CAM_MOVING` bit 5, `ANGLES_US` 40 000, `CUT_ROWS` 2, `MAX_RANGE_M` 4.0, `GATE_D2` 9.21;
  - `ACCEL_SIGMA` 1.0, `FORGET_S` 20, `LOOK_S` 1.0, `REMEMBER_TAU_S` 1.0, `NEW_SPEED_SIGMA` 0.5;
  - σ(d) = 0.02 + 0.03 d²;
  - `FLOOR_CLASSES` = person, cat, dog, sports ball, teddy bear, chair, couch, backpack, suitcase, potted plant.
- **Pictures must be the calibration's size** (QVGA 320 × 240); otherwise skipped as "picture size".
- **Hardware:** the floor checks with the owner present ("stand or floor?" first, saying what moves). Recordings stay local and are deleted after the numbers are written down (they show the home).

## Review Focus
1. **A person whose feet are below the picture's bottom edge** (standing close) must be "near" with a direction, never located at a wrong floor point. Covered by `test_a_box_cut_by_the_bottom_edge_has_only_a_direction`.
2. **Two things of the same class close together** must not swap or merge: the greedy match takes the smallest distance first and uses each once. Covered by `test_two_balls_are_two_things` (60 cm apart). Closer pairs are left to the floor check.
3. **A remembered thing seen again after a long time:** its grown uncertainty must still let it match (the same oid), and its faded speed must not have carried it away. Covered by `test_out_of_view_it_is_remembered_and_comes_back_the_same` (12 s).
4. **A blurred picture or a short miss while the thing is in view** must not drop it at once (the 1 s look check). Covered by `test_a_short_miss_in_view_keeps_it`.
5. **The live loop** must never queue pictures (latest wins, LLD §6) and must stop cleanly on Ctrl-C. Checked by reading `run_live` and on the tank, because it can't be host-tested.

---

### Task 1: The gate, the floor trick and the list (pure)

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/objects.py`, `ai-engine/tests/test_objects.py`
- Modify: `ai-engine/src/conqueror_ai/domain/types.py` (`Detection`, `ObjectEstimate`)

**Interfaces:**
- Consumes: `CameraModel` (`for_frame`, `pixel_to_floor`, `pixel_ray`, `floor_to_pixel`, `sees`, `position`), `from_servo`, `PoseFilter` / `PoseHistory` (Plan U).
- Produces:
  - `Detector` (Protocol: `detect(rgb) -> list[Detection]`);
  - `CameraAngles`, `FrameGate(cal, history, angles).camera_for(cap_us) -> CameraModel | str`, `ObjectLocator().locate(dets, cam, t_us) -> (list[Located], list[Near])`, `ObjectTracker().update(located, cam, t_us)` / `.objects()`;
  - `ObjectPipeline(cal)`: `.on_calib(n)`, `.on_telem(s)`, `.on_frame(frame, size, dets) -> CameraModel | str`, `.objects()`, `.near()`, `.skips()`, `.pose()`;
  - `sigma_at(d)`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/ai-engine/tests/test_objects.py b/ai-engine/tests/test_objects.py
new file mode 100644
index 0000000..95ffeba
--- /dev/null
+++ b/ai-engine/tests/test_objects.py
@@ -0,0 +1,214 @@
+"""Phase 4: things in the room (AI engine LLD §15) - the gate, the floor trick, the list. No model, no tank:
+detections are made by projecting known floor points through the camera model."""
+
+from __future__ import annotations
+
+import math
+
+from conqueror_ai.domain import objects as ob
+from conqueror_ai.domain.camera import CameraModel
+from conqueror_ai.domain.pose import PoseHistory
+from conqueror_ai.domain.types import Detection, Frame, Pose, TelemSample
+from tests.test_camera import calib
+
+CAL = calib(tilt0=-15.0)  # looking down like the driving tilt
+
+
+def cam_at(x: float = 0.0, y: float = 0.0, th_deg: float = 0.0, pan: float = 0.0) -> CameraModel:
+    return CameraModel(CAL).for_frame(Pose(0, x, y, math.radians(th_deg), 0.0, 0.0), pan, 0.0)
+
+
+def box_on_floor(
+    cam: CameraModel, x: float, y: float, cls: str = "sports ball", h_px: float = 20.0
+) -> Detection:
+    """A detection whose bottom middle is where the floor point (x, y) is in the picture."""
+    q = cam.floor_to_pixel(x, y)
+    assert q is not None
+    u, v = q
+    return Detection(cls, 0.9, (u - 10.0, v - h_px, u + 10.0, v))
+
+
+def sample(
+    t_us: int, rate: float = 0.0, flags: int = 0, pan: float = 90.0, tilt: float = 90.0
+) -> TelemSample:
+    return TelemSample(t_us, t_us // 1000, 0, 0, 0.0, rate, None, pan, tilt, 0, 0, flags, 0, 0)
+
+
+# ---- the floor trick
+def test_the_floor_trick_finds_the_floor_point() -> None:
+    cam = cam_at()
+    located, near = ob.ObjectLocator().locate([box_on_floor(cam, 1.0, 0.2)], cam, 0)
+    assert near == [] and len(located) == 1
+    assert math.isclose(located[0].x, 1.0, abs_tol=1e-6) and math.isclose(located[0].y, 0.2, abs_tol=1e-6)
+
+
+def test_the_floor_trick_works_from_any_pose() -> None:
+    cam = cam_at(2.0, 1.0, 90.0)  # the tank at (2, 1), facing left
+    located, _ = ob.ObjectLocator().locate([box_on_floor(cam, 2.0, 2.0)], cam, 0)
+    assert math.isclose(located[0].x, 2.0, abs_tol=1e-6) and math.isclose(located[0].y, 2.0, abs_tol=1e-6)
+
+
+def test_a_box_cut_by_the_bottom_edge_has_only_a_direction() -> None:
+    cam = cam_at()
+    d = Detection("person", 0.9, (200.0, 100.0, 260.0, 239.5))  # right of centre, cut at the bottom
+    located, near = ob.ObjectLocator().locate([d], cam, 7)
+    assert located == [] and len(near) == 1
+    assert near[0].cls == "person" and near[0].bearing_rad < 0.0 and near[0].t_us == 7  # right = clockwise
+
+
+def test_table_things_and_far_or_high_boxes_are_ignored() -> None:
+    cam = cam_at()
+    cup = box_on_floor(cam, 1.0, 0.0, cls="cup")
+    far = box_on_floor(cam, 4.5, 0.0)
+    high = Detection("sports ball", 0.9, (150.0, 0.0, 170.0, 5.0))  # its bottom above the horizon
+    located, near = ob.ObjectLocator().locate([cup, far, high], cam, 0)
+    assert located == [] and near == []
+
+
+def test_the_expected_error_grows_with_the_distance() -> None:
+    assert math.isclose(ob.sigma_at(1.0), 0.05) and math.isclose(ob.sigma_at(2.0), 0.14)
+
+
+# ---- which pictures are used
+def gate_with(samples: list[TelemSample], pose_w: float = 0.0) -> ob.FrameGate:
+    h, a = PoseHistory(), ob.CameraAngles()
+    for s in samples:
+        h.push(Pose(s.bus_us, 0.0, 0.0, 0.0, 0.0, pose_w))
+        a.push(s)
+    return ob.FrameGate(CAL, h, a)
+
+
+def test_the_gate_gives_the_camera_at_the_capture_time() -> None:
+    g = gate_with([sample(1_000_000, pan=80.0), sample(1_020_000, pan=80.0)])  # servo 80 = 10 deg right
+    cam = g.camera_for(1_010_000)
+    assert isinstance(cam, CameraModel) and math.isclose(cam.pan_deg, 10.0)
+
+
+def test_the_gate_skips_with_its_reason() -> None:
+    assert gate_with([]).camera_for(1_000_000) == "no pose"
+    assert (
+        gate_with([sample(1_000_000), sample(1_020_000)], pose_w=math.radians(60)).camera_for(1_010_000)
+        == "turning"
+    )
+    moving = [sample(1_000_000, flags=ob.CAM_MOVING), sample(1_020_000, flags=ob.CAM_MOVING)]
+    assert gate_with(moving).camera_for(1_010_000) == "camera moving"
+    h = PoseHistory()
+    h.push(Pose(1_000_000, 0, 0, 0, 0, 0))
+    h.push(Pose(1_100_000, 0, 0, 0, 0, 0))
+    assert ob.FrameGate(CAL, h, ob.CameraAngles()).camera_for(1_050_000) == "no camera angles"
+
+
+def test_camera_angles_take_the_nearest_sample_within_40_ms() -> None:
+    a = ob.CameraAngles()
+    a.push(sample(1_000_000, pan=70.0))
+    a.push(sample(1_020_000, pan=75.0))
+    got = a.at(1_015_000)
+    assert got is not None and got[0] == 75.0
+    assert a.at(1_100_000) is None
+
+
+# ---- the list
+def track_frames(
+    tr: ob.ObjectTracker,
+    cam: CameraModel,
+    points: list[tuple[float, float]],
+    n: int,
+    t0_us: int = 0,
+    step_us: int = 100_000,
+) -> int:
+    loc = ob.ObjectLocator()
+    t = t0_us
+    for i in range(n):
+        t = t0_us + i * step_us
+        located, _ = loc.locate([box_on_floor(cam, x, y) for x, y in points], cam, t)
+        tr.update(located, cam, t)
+    return t
+
+
+def test_one_ball_seen_ten_times_is_one_thing() -> None:
+    tr, cam = ob.ObjectTracker(), cam_at()
+    track_frames(tr, cam, [(1.0, 0.1)], 10)
+    objs = tr.objects()
+    assert len(objs) == 1 and objs[0].state == "seen"
+    assert math.isclose(objs[0].x_m, 1.0, abs_tol=0.01) and math.isclose(objs[0].y_m, 0.1, abs_tol=0.01)
+
+
+def test_two_balls_are_two_things() -> None:
+    tr, cam = ob.ObjectTracker(), cam_at()
+    track_frames(tr, cam, [(1.0, 0.3), (1.0, -0.3)], 10)
+    assert len(tr.objects()) == 2
+
+
+def test_a_walking_person_is_one_thing_with_its_speed() -> None:
+    tr, cam, loc = ob.ObjectTracker(), cam_at(), ob.ObjectLocator()
+    for i in range(20):  # 0.5 m/s across the view, 10 pictures a second
+        t = i * 100_000
+        y = 0.4 - 0.05 * i
+        located, _ = loc.locate([box_on_floor(cam, 1.5, y, cls="person", h_px=80.0)], cam, t)
+        tr.update(located, cam, t)
+    objs = tr.objects()
+    assert len(objs) == 1 and math.isclose(objs[0].vy, -0.5, abs_tol=0.1)
+
+
+def test_out_of_view_it_is_remembered_and_comes_back_the_same() -> None:
+    tr = ob.ObjectTracker()
+    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
+    oid = tr.objects()[0].oid
+    away = cam_at(th_deg=120.0)  # turned away: the ball is behind
+    t = track_frames(tr, away, [], 120, t0_us=t + 100_000)  # 12 s
+    objs = tr.objects()
+    assert len(objs) == 1 and objs[0].state == "remembered" and objs[0].oid == oid
+    track_frames(tr, cam_at(), [(1.0, 0.0)], 3, t0_us=t + 100_000)
+    objs = tr.objects()
+    assert len(objs) == 1 and objs[0].state == "seen" and objs[0].oid == oid
+
+
+def test_remembered_for_20_s_it_is_forgotten() -> None:
+    tr = ob.ObjectTracker()
+    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
+    track_frames(tr, cam_at(th_deg=120.0), [], 205, t0_us=t + 100_000)  # 20.5 s
+    assert tr.objects() == []
+
+
+def test_its_spot_in_view_for_1_s_without_it_drops_it() -> None:
+    tr = ob.ObjectTracker()
+    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
+    track_frames(tr, cam_at(), [], 12, t0_us=t + 100_000)  # the camera looks right at it: 1.2 s, nothing
+    assert tr.objects() == []
+
+
+def test_a_short_miss_in_view_keeps_it() -> None:
+    tr = ob.ObjectTracker()
+    t = track_frames(tr, cam_at(), [(1.0, 0.0)], 5)
+    t = track_frames(tr, cam_at(), [], 3, t0_us=t + 100_000)  # 0.3 s missed (a blurred picture)
+    track_frames(tr, cam_at(), [(1.0, 0.0)], 2, t0_us=t + 100_000)
+    assert len(tr.objects()) == 1
+
+
+# ---- the pipeline
+def frame(cap_us: int) -> Frame:
+    return Frame(seq=cap_us // 1000, cap_us=cap_us, recv_s=cap_us / 1e6 + 0.1, jpeg=b"")
+
+
+def run_pipeline() -> list[tuple[int, str, float, float]]:
+    p = ob.ObjectPipeline(CAL)
+    p.on_calib(7514)
+    cam = cam_at()
+    for i in range(60):  # 1.2 s of telemetry, the tank still
+        p.on_telem(TelemSample(i * 20_000, i * 20, 0, 0, 0.0, 0.0, None, 90.0, 90.0, 0, 0, 0, 0, 0))
+        if i % 5 == 4:
+            p.on_frame(frame(i * 20_000 - 10_000), (320, 240), [box_on_floor(cam, 1.0, 0.1)])
+    return [(o.oid, o.cls, round(o.x_m, 4), round(o.y_m, 4)) for o in p.objects()]
+
+
+def test_the_pipeline_lists_the_ball_and_replays_the_same() -> None:
+    first = run_pipeline()
+    assert len(first) == 1 and first[0][1] == "sports ball" and math.isclose(first[0][2], 1.0, abs_tol=0.01)
+    assert run_pipeline() == first
+
+
+def test_the_pipeline_counts_its_skips() -> None:
+    p = ob.ObjectPipeline(CAL)
+    p.on_frame(frame(1_000_000), (320, 240), [])
+    p.on_frame(frame(1_000_000), (640, 480), [])
+    assert p.skips() == {"no pose": 1, "picture size": 1}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_objects.py`
Expected: `ImportError: cannot import name 'objects' from 'conqueror_ai.domain'`.

- [ ] **Step 3: Write the code** (apply this diff):

```diff
diff --git a/ai-engine/src/conqueror_ai/domain/objects.py b/ai-engine/src/conqueror_ai/domain/objects.py
new file mode 100644
index 0000000..7733622
--- /dev/null
+++ b/ai-engine/src/conqueror_ai/domain/objects.py
@@ -0,0 +1,309 @@
+"""Phase 4: things in the room (AI engine LLD §15, design-notes §134). Pure: detections in, a list out.
+
+- FrameGate: the camera for a picture at its capture time - or why the picture is skipped.
+- ObjectLocator: the floor trick - a box's bottom middle is where the thing touches the floor.
+- ObjectTracker: one constant-velocity Kalman filter per thing, matched in room positions; Seen / Remembered;
+  forgotten after 20 s unseen, or when its spot is in view for 1 s without it.
+- ObjectPipeline: telemetry and pictures in order; the detector runs outside (a worker thread)."""
+
+from __future__ import annotations
+
+import math
+from collections import Counter, deque
+from dataclasses import dataclass, field
+from typing import Protocol
+
+import numpy as np
+from numpy.typing import NDArray
+
+from conqueror_ai.domain.camera import Calibration, CameraModel, from_servo
+from conqueror_ai.domain.pose import PoseFilter, PoseHistory
+from conqueror_ai.domain.types import Detection, Frame, ObjectEstimate, Pose, TelemSample
+
+GATE_DPS = 45.0  # turning faster blurs the picture
+CAM_MOVING = 1 << 5  # FastFlagBit CAM_MOVING
+ANGLES_US = 40_000  # the camera angles must be this close to the capture time
+HISTORY_US = 2_000_000
+FLOOR_CLASSES = frozenset(
+    {
+        "person",
+        "cat",
+        "dog",
+        "sports ball",
+        "teddy bear",
+        "chair",
+        "couch",
+        "backpack",
+        "suitcase",
+        "potted plant",
+    }
+)
+CUT_ROWS = 2  # a box this near the picture's bottom edge is cut off: a direction only
+MAX_RANGE_M = 4.0
+GATE_D2 = 9.21  # chi-square, 2 numbers, 99 %
+ACCEL_SIGMA = 1.0  # m/s^2: how fast a thing may change its speed
+FORGET_S = 20.0
+LOOK_S = 1.0
+REMEMBER_TAU_S = 1.0  # a remembered thing's speed fades: it has probably stopped
+NEW_SPEED_SIGMA = 0.5  # m/s: a new thing's speed is unknown
+
+FloatArray = NDArray[np.float64]
+
+
+class Detector(Protocol):
+    """The detector port (LLD §15.1): YoloCoreml on the tank's pictures, a fake in tests."""
+
+    def detect(self, rgb: NDArray[np.uint8]) -> list[Detection]: ...
+
+
+def sigma_at(d: float) -> float:
+    """The floor trick's expected error at d metres from the camera (Spec 7 §7: ~5 cm at 1 m, ~15 at 2 m)."""
+    return 0.02 + 0.03 * d * d
+
+
+class CameraAngles:
+    """The last 2 s of the camera servos (telemetry), for the picture's capture time."""
+
+    def __init__(self) -> None:
+        self._s: deque[tuple[int, float, float, int]] = deque()
+
+    def push(self, s: TelemSample) -> None:
+        if self._s and s.bus_us < self._s[-1][0]:
+            self._s.clear()  # the bridge restarted
+        self._s.append((s.bus_us, s.pan_deg, s.tilt_deg, s.flags))
+        while s.bus_us - self._s[0][0] > HISTORY_US:
+            self._s.popleft()
+
+    def at(self, t_us: int) -> tuple[float, float, int] | None:
+        """(pan, tilt, flags) of the nearest sample within 40 ms, or None."""
+        best = min(self._s, key=lambda e: abs(e[0] - t_us), default=None)
+        if best is None or abs(best[0] - t_us) > ANGLES_US:
+            return None
+        return best[1], best[2], best[3]
+
+
+class FrameGate:
+    def __init__(self, cal: Calibration, history: PoseHistory, angles: CameraAngles) -> None:
+        self.cal = cal
+        self._history = history
+        self._angles = angles
+
+    def capture_us(self, cap_us: int) -> int:
+        """The middle of the picture's readout (LLD §5.4)."""
+        return cap_us + self.cal.readout_us // 2
+
+    def camera_for(self, cap_us: int) -> CameraModel | str:
+        """The camera model at the capture time, or the reason the picture is skipped."""
+        t = self.capture_us(cap_us)
+        pose = self._history.at(t)
+        if pose is None:
+            return "no pose"
+        a = self._angles.at(t)
+        if a is None:
+            return "no camera angles"
+        if a[2] & CAM_MOVING:
+            return "camera moving"
+        if abs(math.degrees(pose.w_rps)) > GATE_DPS:
+            return "turning"
+        return CameraModel(self.cal).for_frame(pose, *from_servo(a[0], a[1]))
+
+
+@dataclass(frozen=True)
+class Located:
+    cls: str
+    x: float
+    y: float
+    sigma: float
+    box: tuple[float, float, float, float]
+
+
+@dataclass(frozen=True)
+class Near:
+    """A thing cut off by the picture's bottom edge: too near for the floor trick, a direction only."""
+
+    cls: str
+    bearing_rad: float  # in the room (counter-clockwise from x)
+    t_us: int
+
+
+class ObjectLocator:
+    def locate(self, dets: list[Detection], cam: CameraModel, t_us: int) -> tuple[list[Located], list[Near]]:
+        located: list[Located] = []
+        near: list[Near] = []
+        h = cam.cal.size[1]
+        c = cam.position()
+        for d in dets:
+            if d.cls not in FLOOR_CLASSES:
+                continue
+            x1, y1, x2, y2 = d.box
+            u = (x1 + x2) / 2.0
+            if y2 >= h - CUT_ROWS:
+                ray = cam.pixel_ray(u, (y1 + y2) / 2.0)
+                if ray is not None:
+                    near.append(Near(d.cls, math.atan2(float(ray[1]), float(ray[0])), t_us))
+                continue
+            p = cam.pixel_to_floor(u, y2)
+            if p is None:
+                continue
+            dist = math.dist((p[0], p[1], 0.0), (float(c[0]), float(c[1]), float(c[2])))
+            if dist > MAX_RANGE_M:
+                continue
+            located.append(Located(d.cls, p[0], p[1], sigma_at(dist), d.box))
+        return located, near
+
+
+_H = np.array([[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0]])
+
+
+@dataclass
+class _Thing:
+    oid: int
+    cls: str
+    x: FloatArray  # x, y, vx, vy
+    p: FloatArray  # 4 x 4
+    t_us: int
+    first_us: int
+    last_us: int
+    state: str = "seen"
+    in_view_since: int | None = None
+
+    def predict(self, t_us: int) -> None:
+        dt = (t_us - self.t_us) / 1e6
+        if dt <= 0:
+            return
+        if self.state == "remembered":
+            self.x[2:] *= math.exp(-dt / REMEMBER_TAU_S)
+        f = np.eye(4)
+        f[0, 2] = f[1, 3] = dt
+        g = np.array([[dt * dt / 2, 0.0], [0.0, dt * dt / 2], [dt, 0.0], [0.0, dt]])
+        self.x = f @ self.x
+        self.p = f @ self.p @ f.T + g @ g.T * ACCEL_SIGMA**2
+        self.t_us = t_us
+
+    def d2(self, z: Located) -> float:
+        s = _H @ self.p @ _H.T + np.eye(2) * z.sigma**2
+        y = np.array([z.x, z.y]) - _H @ self.x
+        return float(y @ np.linalg.solve(s, y))
+
+    def update(self, z: Located) -> None:
+        s = _H @ self.p @ _H.T + np.eye(2) * z.sigma**2
+        k = self.p @ _H.T @ np.linalg.inv(s)
+        self.x = self.x + k @ (np.array([z.x, z.y]) - _H @ self.x)
+        self.p = (np.eye(4) - k @ _H) @ self.p
+
+    def estimate(self) -> ObjectEstimate:
+        sigma = math.sqrt(float(np.linalg.eigvalsh(self.p[:2, :2]).max()))
+        return ObjectEstimate(
+            self.oid,
+            self.cls,
+            float(self.x[0]),
+            float(self.x[1]),
+            float(self.x[2]),
+            float(self.x[3]),
+            sigma,
+            self.first_us,
+            self.last_us,
+            self.state,
+        )
+
+
+@dataclass
+class ObjectTracker:
+    _things: list[_Thing] = field(default_factory=lambda: list[_Thing]())
+    _next: int = 1
+
+    def update(self, located: list[Located], cam: CameraModel, t_us: int) -> None:
+        for th in self._things:
+            th.predict(t_us)
+        pairs = sorted(
+            (th.d2(z), i, j)
+            for i, th in enumerate(self._things)
+            for j, z in enumerate(located)
+            if th.cls == z.cls
+        )
+        used_t: set[int] = set()
+        used_z: set[int] = set()
+        for d2, i, j in pairs:
+            if d2 > GATE_D2 or i in used_t or j in used_z:
+                continue
+            used_t.add(i)
+            used_z.add(j)
+            th = self._things[i]
+            th.update(located[j])
+            th.state, th.last_us, th.in_view_since = "seen", t_us, None
+        keep: list[_Thing] = []
+        for i, th in enumerate(self._things):
+            if i not in used_t and not self._still_there(th, cam, t_us):
+                continue
+            keep.append(th)
+        for j, z in enumerate(located):
+            if j not in used_z:
+                p = np.diag([z.sigma**2, z.sigma**2, NEW_SPEED_SIGMA**2, NEW_SPEED_SIGMA**2])
+                keep.append(_Thing(self._next, z.cls, np.array([z.x, z.y, 0.0, 0.0]), p, t_us, t_us, t_us))
+                self._next += 1
+        self._things = keep
+
+    @staticmethod
+    def _still_there(th: _Thing, cam: CameraModel, t_us: int) -> bool:
+        """An unmatched thing: kept unless forgotten, or its spot has been in view for 1 s without it."""
+        if (t_us - th.last_us) / 1e6 >= FORGET_S:
+            return False
+        if cam.sees(float(th.x[0]), float(th.x[1])):
+            if th.in_view_since is None:
+                th.in_view_since = t_us
+            return (t_us - th.in_view_since) / 1e6 < LOOK_S
+        th.state, th.in_view_since = "remembered", None
+        return True
+
+    def objects(self) -> list[ObjectEstimate]:
+        return [th.estimate() for th in self._things]
+
+
+class ObjectPipeline:
+    """Telemetry and pictures in arrival order -> the list. The detector is run outside and handed in."""
+
+    def __init__(self, cal: Calibration) -> None:
+        self.cal = cal
+        self._pose = PoseFilter()
+        self._history = PoseHistory()
+        self._angles = CameraAngles()
+        self.gate = FrameGate(cal, self._history, self._angles)
+        self._locator = ObjectLocator()
+        self.tracker = ObjectTracker()
+        self._near: list[Near] = []
+        self._skips: Counter[str] = Counter()
+        self.last_camera: CameraModel | None = None
+
+    def on_calib(self, counts_per_m: int) -> None:
+        self._pose.set_counts_per_m(counts_per_m)
+
+    def on_telem(self, s: TelemSample) -> None:
+        self._history.push(self._pose.update(s))
+        self._angles.push(s)
+
+    def on_frame(self, f: Frame, size: tuple[int, int], dets: list[Detection]) -> CameraModel | str:
+        """Use one picture (its decoded size, its detections): its camera, or why it was skipped."""
+        if size != self.cal.size:
+            self._skips["picture size"] += 1
+            return "picture size"
+        cam = self.gate.camera_for(f.cap_us)
+        if isinstance(cam, str):
+            self._skips[cam] += 1
+            return cam
+        t = self.gate.capture_us(f.cap_us)
+        located, self._near = self._locator.locate(dets, cam, t)
+        self.tracker.update(located, cam, t)
+        self.last_camera = cam
+        return cam
+
+    def objects(self) -> list[ObjectEstimate]:
+        return self.tracker.objects()
+
+    def near(self) -> list[Near]:
+        return list(self._near)
+
+    def skips(self) -> dict[str, int]:
+        return dict(self._skips)
+
+    def pose(self) -> Pose | None:
+        return self._pose.pose()
diff --git a/ai-engine/src/conqueror_ai/domain/types.py b/ai-engine/src/conqueror_ai/domain/types.py
index 87d3b0f..9b3d016 100644
--- a/ai-engine/src/conqueror_ai/domain/types.py
+++ b/ai-engine/src/conqueror_ai/domain/types.py
@@ -46,3 +46,28 @@ class Pose:
     th_rad: float
     v_mps: float
     w_rps: float
+
+
+@dataclass(frozen=True)
+class Detection:
+    """One box from the detector (LLD §15.1), in the picture's pixels."""
+
+    cls: str
+    score: float
+    box: tuple[float, float, float, float]  # x1, y1, x2, y2
+
+
+@dataclass(frozen=True)
+class ObjectEstimate:
+    """One thing on the list (LLD §15.4), in the room frame."""
+
+    oid: int
+    cls: str
+    x_m: float
+    y_m: float
+    vx: float
+    vy: float
+    sigma_m: float  # the position's standard deviation (the larger axis)
+    first_seen_us: int
+    last_seen_us: int
+    state: str  # "seen" or "remembered"
```

- [ ] **Step 4: Run the tests and the checks**

Run: `cd ai-engine && uv run pytest -q && cd ai-engine && uv run ruff check . && cd ai-engine && uv run ruff format --check . && cd ai-engine && uv run pyright`
Expected: `17 passed` in `test_objects.py`, 127 in all; ruff and pyright clean.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/domain ai-engine/tests/test_objects.py
git commit -m "feat(ai): phase 4 - the gate, the floor trick and the list of things (AI engine LLD §15.2-15.4)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 2: The detector (CoreML) and its model

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/yolo_coreml.py`, `ai-engine/tests/test_yolo_coreml.py`, `tools/measure/export_yolo.py`
- Modify: `ai-engine/.gitignore` (`models/`); `ai-engine/pyproject.toml`, adding to `dependencies` after `"pyyaml>=6,<7",`:
  ```toml
    "coremltools>=8,<10",  # the detector at run time (AI engine LLD §15.1); the export lives in tools/measure
    "pillow>=10",
  ```

**Interfaces:**
- Consumes: Task 1's `Detection`.
- Produces: `YoloCoreml(path, conf=0.35).detect(rgb) -> list[Detection]`; `letterbox(rgb, size)`, `to_detections(rows, pad, size, names, conf)`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/ai-engine/tests/test_yolo_coreml.py b/ai-engine/tests/test_yolo_coreml.py
new file mode 100644
index 0000000..7e7ea4d
--- /dev/null
+++ b/ai-engine/tests/test_yolo_coreml.py
@@ -0,0 +1,51 @@
+"""The CoreML YOLO adapter (AI engine LLD §15.1): the letterbox and the box mapping are pure and tested here;
+the model itself runs only when its file exists (made by tools/measure/export_yolo.py)."""
+
+from __future__ import annotations
+
+from pathlib import Path
+
+import numpy as np
+import pytest
+
+from conqueror_ai.adapters import yolo_coreml as yc
+
+MODEL = Path(__file__).resolve().parents[1] / "models" / "yolo26n-320.mlpackage"
+
+
+def test_a_qvga_picture_is_letterboxed_into_the_middle() -> None:
+    rgb = np.full((240, 320, 3), 200, dtype=np.uint8)
+    img, (top, left, scale) = yc.letterbox(rgb, 320)
+    assert img.shape == (320, 320, 3) and (top, left, scale) == (40, 0, 1.0)
+    assert (img[:40] == 114).all() and (img[280:] == 114).all() and (img[40:280] == 200).all()
+
+
+def test_the_model_rows_become_detections_in_the_picture() -> None:
+    rows = np.array(
+        [
+            [10.0, 60.0, 50.0, 200.0, 0.9, 0.0],  # a person
+            [100.0, 30.0, 140.0, 330.0, 0.6, 32.0],  # a ball, its bottom in the grey bar below
+            [0.0, 0.0, 10.0, 10.0, 0.2, 0.0],  # too unsure
+        ]
+    )
+    names = {0: "person", 32: "sports ball"}
+    dets = yc.to_detections(rows, (40, 0, 1.0), (320, 240), names, 0.35)
+    assert [d.cls for d in dets] == ["person", "sports ball"]
+    assert dets[0].box == (10.0, 20.0, 50.0, 160.0)
+    assert dets[1].box == (100.0, 0.0, 140.0, 240.0)  # moved back by 40 and clamped to the picture
+
+
+def test_no_model_file_is_a_plain_refusal(tmp_path: Path) -> None:
+    with pytest.raises(FileNotFoundError, match="export_yolo"):
+        yc.YoloCoreml(tmp_path / "missing.mlpackage")
+
+
+@pytest.mark.skipif(not MODEL.exists(), reason="no model file (tools/measure/export_yolo.py makes it)")
+def test_the_model_runs_on_a_picture() -> None:
+    det = yc.YoloCoreml(MODEL)
+    rgb = np.full((240, 320, 3), 128, dtype=np.uint8)
+    out = det.detect(rgb)
+    assert isinstance(out, list)
+    for d in out:
+        x1, y1, x2, y2 = d.box
+        assert 0 <= x1 <= x2 <= 320 and 0 <= y1 <= y2 <= 240
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_yolo_coreml.py`
Expected: `ImportError: cannot import name 'yolo_coreml'`.

- [ ] **Step 3: Write the code** (apply this diff), and add the two dependencies to `pyproject.toml`:

```diff
diff --git a/ai-engine/.gitignore b/ai-engine/.gitignore
index 3a27dff..d1f87da 100644
--- a/ai-engine/.gitignore
+++ b/ai-engine/.gitignore
@@ -3,3 +3,4 @@ recordings/
 __pycache__/
 *.rrd
 config/checkerboard.png
+models/
diff --git a/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py b/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py
new file mode 100644
index 0000000..0b54c31
--- /dev/null
+++ b/ai-engine/src/conqueror_ai/adapters/yolo_coreml.py
@@ -0,0 +1,85 @@
+"""The detector (AI engine LLD §15.1): YOLO26n exported to CoreML, on the Neural Engine.
+
+The model is made once by tools/measure/export_yolo.py (whose environment has Ultralytics and torch) into
+ai-engine/models/ (git-ignored: about 5 MB, AGPL). Here only Apple's CoreML runtime is needed. The export is
+end-to-end: rows of x1, y1, x2, y2, score, class in the model's square input - no NMS to do."""
+
+from __future__ import annotations
+
+import ast
+from pathlib import Path
+from typing import Any
+
+import numpy as np
+from numpy.typing import NDArray
+
+from conqueror_ai.domain.types import Detection
+
+CONF = 0.35
+GREY = 114  # the letterbox's padding, as Ultralytics pads
+
+
+def letterbox(rgb: NDArray[np.uint8], size: int) -> tuple[NDArray[np.uint8], tuple[int, int, float]]:
+    """The picture scaled to fit a size x size square and centred on grey; (top, left, scale) to undo it."""
+    import cv2
+
+    h, w = rgb.shape[:2]
+    scale = min(size / w, size / h)
+    nw, nh = round(w * scale), round(h * scale)
+    img = rgb if (nw, nh) == (w, h) else cv2.resize(rgb, (nw, nh), interpolation=cv2.INTER_LINEAR)
+    out = np.full((size, size, 3), GREY, dtype=np.uint8)
+    top, left = (size - nh) // 2, (size - nw) // 2
+    out[top : top + nh, left : left + nw] = img
+    return out, (top, left, float(scale))
+
+
+def to_detections(
+    rows: NDArray[np.float64],
+    pad: tuple[int, int, float],
+    size: tuple[int, int],
+    names: dict[int, str],
+    conf: float,
+) -> list[Detection]:
+    """Model rows -> detections in the picture's pixels (letterbox undone, clamped to the picture)."""
+    top, left, scale = pad
+    w, h = size
+    out: list[Detection] = []
+    for x1, y1, x2, y2, score, cls in rows.tolist():
+        if score < conf:
+            continue
+        name = names.get(int(cls))
+        if name is None:
+            continue
+        bx = (
+            min(max((x1 - left) / scale, 0.0), float(w)),
+            min(max((y1 - top) / scale, 0.0), float(h)),
+            min(max((x2 - left) / scale, 0.0), float(w)),
+            min(max((y2 - top) / scale, 0.0), float(h)),
+        )
+        out.append(Detection(name, float(score), bx))
+    return out
+
+
+class YoloCoreml:
+    def __init__(self, path: Path, conf: float = CONF) -> None:
+        if not path.exists():
+            raise FileNotFoundError(f"no detector model at {path} - run tools/measure/export_yolo.py")
+        import coremltools as ct  # pyright: ignore[reportMissingTypeStubs]
+
+        self._ct: Any = ct
+        self._model: Any = ct.models.MLModel(str(path), compute_units=ct.ComputeUnit.ALL)
+        spec: Any = self._model.get_spec()
+        meta: dict[str, str] = dict(spec.description.metadata.userDefined)
+        self._names: dict[int, str] = ast.literal_eval(meta["names"])
+        self._size = int(ast.literal_eval(meta["imgsz"])[0])
+        self._input = str(spec.description.input[0].name)
+        self._output = str(spec.description.output[0].name)
+        self._conf = conf
+
+    def detect(self, rgb: NDArray[np.uint8]) -> list[Detection]:
+        from PIL import Image
+
+        img, pad = letterbox(rgb, self._size)
+        res: dict[str, Any] = self._model.predict({self._input: Image.fromarray(img)})
+        rows = np.asarray(res[self._output], dtype=np.float64).reshape(-1, 6)
+        return to_detections(rows, pad, (rgb.shape[1], rgb.shape[0]), self._names, self._conf)
diff --git a/tools/measure/export_yolo.py b/tools/measure/export_yolo.py
new file mode 100644
index 0000000..a4db029
--- /dev/null
+++ b/tools/measure/export_yolo.py
@@ -0,0 +1,36 @@
+"""Make the AI engine's detector model (AI engine LLD §15.1): YOLO26n exported to CoreML at 320 px, end-to-end
+(no NMS), half precision - the same export phase 0 measured (design-notes §109) - copied to ai-engine/models/.
+It needs this environment's Ultralytics and torch 2.7; the engine itself needs only the CoreML runtime.
+
+    cd tools/measure && uv run python export_yolo.py
+"""
+
+from __future__ import annotations
+
+import argparse
+import shutil
+from pathlib import Path
+
+from ne_budget import yolo_coreml  # exports once into tools/measure/models/, then reuses it
+
+HERE = Path(__file__).resolve().parent
+ENGINE_MODELS = HERE.parents[1] / "ai-engine" / "models"
+
+
+def main() -> None:
+    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
+    ap.add_argument("--yolo", default="yolo26n")
+    ap.add_argument("--imgsz", type=int, default=320)
+    a = ap.parse_args()
+    yolo_coreml(a.yolo, a.imgsz)
+    src = HERE / "models" / f"{a.yolo}-{a.imgsz}.mlpackage"
+    dst = ENGINE_MODELS / src.name
+    ENGINE_MODELS.mkdir(parents=True, exist_ok=True)
+    if dst.exists():
+        shutil.rmtree(dst)
+    shutil.copytree(src, dst)
+    print(f"detector model: {dst}")
+
+
+if __name__ == "__main__":
+    main()
```

- [ ] **Step 4: Install, make the model, run the tests and the checks**

Run:
- `cd ai-engine && uv sync -q`
- `cd tools/measure && uv run python export_yolo.py`
  - Expected: `detector model: …/ai-engine/models/yolo26n-320.mlpackage`. It reuses `tools/measure/models/yolo26n-320.mlpackage` from phase 0 if it's there.
- `cd ai-engine && uv run pytest -q tests/test_yolo_coreml.py`
  - Expected: `4 passed`; the model test runs now.
- `cd ai-engine && uv run pytest -q && cd ai-engine && uv run ruff check . && cd ai-engine && uv run ruff format --check . && cd ai-engine && uv run pyright`
  - Expected: all pass; clean.

- [ ] **Step 5: Commit** (`uv.lock` too; never the model)

```bash
git add ai-engine/src/conqueror_ai/adapters/yolo_coreml.py ai-engine/tests/test_yolo_coreml.py ai-engine/.gitignore ai-engine/pyproject.toml ai-engine/uv.lock tools/measure/export_yolo.py
git commit -m "feat(ai): the detector - YOLO26n on CoreML, and its export script (AI engine LLD §15.1)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: `cq-objects` — replay and live

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/objects.py`, `ai-engine/tests/test_objects_app.py`
- Modify: `ai-engine/src/conqueror_ai/adapters/rerun_view.py` (`detections`, `plan`); `ai-engine/pyproject.toml`, adding to `[project.scripts]` after `cq-pose = …`:
  ```toml
  cq-objects = "conqueror_ai.app.objects:main"
  ```

**Interfaces:**
- Consumes:
  - Task 1's `ObjectPipeline`, `Detector`;
  - Task 2's `YoloCoreml`;
  - `adapters.session.replay`, `BridgeLink`, `MjpegSource` (`next_frame` waits for a newer picture: latest wins), `calibration_store.load(path, (320, 240))`.
- Produces: `run_replay(folder, cal, det) -> ReplayResult`, `report(res) -> str`, `run_live(host, cal, det)`, `decode(frame)`; `RerunView.detections(f, dets)`, `RerunView.plan(t_us, pose, objs, skipped)`.

- [ ] **Step 1: Write the failing tests** (apply this diff):

```diff
diff --git a/ai-engine/tests/test_objects_app.py b/ai-engine/tests/test_objects_app.py
new file mode 100644
index 0000000..48abb10
--- /dev/null
+++ b/ai-engine/tests/test_objects_app.py
@@ -0,0 +1,99 @@
+"""`cq-objects <recording>` (AI engine LLD §15.6): a recorded session through the detector and the list,
+here with a fake detector that 'sees' a ball on the floor 1 m ahead."""
+
+from __future__ import annotations
+
+import json
+import math
+from pathlib import Path
+
+import cv2
+import numpy as np
+from numpy.typing import NDArray
+
+from conqueror_ai.adapters.session import SessionRecorder
+from conqueror_ai.app import objects as app
+from conqueror_ai.domain.types import Detection, Frame
+from tests.test_objects import CAL, box_on_floor, cam_at
+
+GREY_JPEG = bytes(cv2.imencode(".jpg", np.full((240, 320, 3), 128, dtype=np.uint8))[1].tobytes())
+
+
+class BallDetector:
+    """Sees a ball 1 m ahead, 10 cm to the left, in every picture."""
+
+    def __init__(self) -> None:
+        self.box = box_on_floor(cam_at(), 1.0, 0.1)
+        self.calls = 0
+
+    def detect(self, rgb: NDArray[np.uint8]) -> list[Detection]:
+        assert rgb.shape == (240, 320, 3)
+        self.calls += 1
+        return [self.box]
+
+
+def td(t_ms: int) -> str:
+    return json.dumps(
+        {
+            "t": "td",
+            "seq": t_ms,
+            "uno_ms": t_ms,
+            "bus_us": t_ms * 1000,
+            "enc": [0, 0],
+            "pwm": [0, 0],
+            "cmd": [0, 0],
+            "yaw": 0.0,
+            "rate": 0.0,
+            "dist_mm": None,
+            "pan": 90.0,
+            "tilt": 90.0,
+            "flags": 0,
+        }
+    )
+
+
+def session(folder: Path) -> None:
+    """2 s: the tank still, telemetry at 50 Hz, a picture every 100 ms (arriving 100 ms after capture)."""
+    rec = SessionRecorder(folder, {"what": "test"})
+    rec.telemetry(0.0, json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27, "gyro_scale": 10310}))
+    for t in range(0, 2000, 20):
+        rec.telemetry(t / 1000, td(t))
+        if t % 100 == 0 and t >= 100:
+            rec.frame(Frame(seq=t, cap_us=(t - 100) * 1000, recv_s=t / 1000, jpeg=GREY_JPEG))
+    rec.close()
+
+
+def test_a_recording_lists_the_ball(tmp_path: Path) -> None:
+    session(tmp_path / "s")
+    det = BallDetector()
+    res = app.run_replay(tmp_path / "s", CAL, det)
+    assert det.calls == 19
+    assert len(res.things) == 1
+    th = next(iter(res.things.values()))
+    assert (
+        th.cls == "sports ball"
+        and math.isclose(th.x_m, 1.0, abs_tol=0.01)
+        and math.isclose(th.y_m, 0.1, abs_tol=0.01)
+    )
+    assert res.used == 19 and res.skips == {}
+
+
+def test_the_report_says_what_was_found_and_skipped(tmp_path: Path) -> None:
+    session(tmp_path / "s")
+    text = app.report(app.run_replay(tmp_path / "s", CAL, BallDetector()))
+    assert "sports ball" in text and "100 cm" in text and "19 pictures used" in text
+
+
+def test_the_live_view_takes_boxes_and_the_plan(tmp_path: Path) -> None:
+    from conqueror_ai.adapters.rerun_view import RerunView
+    from conqueror_ai.domain.types import ObjectEstimate, Pose
+
+    view = RerunView(spawn=False, save=tmp_path / "v.rrd")
+    f = Frame(seq=1, cap_us=1_000_000, recv_s=1.1, jpeg=GREY_JPEG)
+    view.frame(f, None)
+    view.detections(f, [Detection("sports ball", 0.9, (10.0, 20.0, 30.0, 40.0))])
+    seen = ObjectEstimate(1, "sports ball", 1.0, 0.1, 0.0, 0.0, 0.05, 0, 1_000_000, "seen")
+    gone = ObjectEstimate(2, "person", 2.0, -0.5, 0.0, 0.0, 0.3, 0, 500_000, "remembered")
+    view.plan(1_000_000, Pose(1_000_000, 0.0, 0.0, 0.0, 0.0, 0.0), [seen, gone], None)
+    view.plan(1_100_000, None, [], "turning")
+    assert (tmp_path / "v.rrd").exists()
```

- [ ] **Step 2: Run them to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_objects_app.py`
Expected: `ImportError: cannot import name 'objects' from 'conqueror_ai.app'`.

- [ ] **Step 3: Write the code** (apply this diff), and add the `cq-objects` script line:

```diff
diff --git a/ai-engine/src/conqueror_ai/adapters/rerun_view.py b/ai-engine/src/conqueror_ai/adapters/rerun_view.py
index 7d1ef00..041276e 100644
--- a/ai-engine/src/conqueror_ai/adapters/rerun_view.py
+++ b/ai-engine/src/conqueror_ai/adapters/rerun_view.py
@@ -7,7 +7,7 @@ from pathlib import Path
 
 import rerun as rr
 
-from conqueror_ai.domain.types import Frame, TelemSample
+from conqueror_ai.domain.types import Detection, Frame, ObjectEstimate, Pose, TelemSample
 
 
 class RerunView:
@@ -29,3 +29,31 @@ class RerunView:
         rr.log("camera/image", rr.EncodedImage(contents=f.jpeg, media_type="image/jpeg"))
         if age_s is not None:
             rr.log("camera/age_on_arrival_ms", rr.Scalars(age_s * 1000.0))
+
+    def detections(self, f: Frame, dets: list[Detection]) -> None:
+        """The detector's boxes over the picture (AI engine LLD §15.6)."""
+        rr.set_time("bridge", duration=f.cap_us / 1e6)
+        rr.log(
+            "camera/image/boxes",
+            rr.Boxes2D(
+                array=[list(d.box) for d in dets],
+                array_format=rr.Box2DFormat.XYXY,
+                labels=[f"{d.cls} {d.score:.2f}" for d in dets],
+            ),
+        )
+
+    def plan(self, t_us: int, pose: Pose | None, objs: list[ObjectEstimate], skipped: str | None) -> None:
+        """The room from above: the tank and the things (seen: solid, remembered: faded); y left is up."""
+        rr.set_time("bridge", duration=t_us / 1e6)
+        if pose is not None:
+            rr.log("plan/tank", rr.Points2D([[pose.x_m, pose.y_m]], radii=0.08, colors=[[80, 160, 255]]))
+        rr.log(
+            "plan/things",
+            rr.Points2D(
+                [[o.x_m, o.y_m] for o in objs],
+                radii=[max(0.05, o.sigma_m) for o in objs],
+                colors=[[255, 200, 0] if o.state == "seen" else [120, 100, 40] for o in objs],
+                labels=[f"#{o.oid} {o.cls}" for o in objs],
+            ),
+        )
+        rr.log("plan/skipped", rr.TextLog(skipped or "used"))
diff --git a/ai-engine/src/conqueror_ai/app/objects.py b/ai-engine/src/conqueror_ai/app/objects.py
new file mode 100644
index 0000000..f4c8130
--- /dev/null
+++ b/ai-engine/src/conqueror_ai/app/objects.py
@@ -0,0 +1,181 @@
+"""`cq-objects`: the things in the room (AI engine LLD §15.6) - phase 4 of the map. It only watches; it never
+drives.
+
+    uv run cq-objects recordings/20261024-101500      # replay: every picture, in order; prints what was found
+    uv run cq-objects --live                          # the tank: the live view (picture + boxes, the plan)
+"""
+
+from __future__ import annotations
+
+import argparse
+import asyncio
+import contextlib
+import json
+import math
+from dataclasses import dataclass, field
+from pathlib import Path
+from typing import cast
+
+import cv2
+import numpy as np
+from numpy.typing import NDArray
+
+from conqueror_ai.adapters import calibration_store
+from conqueror_ai.adapters.session import replay
+from conqueror_ai.domain.camera import Calibration
+from conqueror_ai.domain.objects import Detector, ObjectPipeline
+from conqueror_ai.domain.telemetry import parse_td
+from conqueror_ai.domain.types import Frame, ObjectEstimate
+
+ENGINE = Path(__file__).resolve().parents[3]
+CALIBRATION = ENGINE / "config" / "calibration.yaml"
+MODEL = ENGINE / "models" / "yolo26n-320.mlpackage"
+DEFAULT_HOST = "192.168.31.167"
+
+
+def decode(f: Frame) -> NDArray[np.uint8] | None:
+    # OpenCV returns None for a broken JPEG (its type hints do not say so)
+    bgr = cast(
+        "NDArray[np.uint8] | None", cv2.imdecode(np.frombuffer(f.jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
+    )
+    if bgr is None:
+        return None
+    return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.uint8)
+
+
+@dataclass
+class Thing:
+    """What the replay says about one thing it found."""
+
+    cls: str
+    first_us: int
+    last_us: int
+    x_m: float
+    y_m: float
+    sigma_m: float
+
+
+@dataclass
+class ReplayResult:
+    things: dict[int, Thing] = field(default_factory=lambda: dict[int, Thing]())
+    used: int = 0
+    skips: dict[str, int] = field(default_factory=lambda: dict[str, int]())
+    start_us: int | None = None
+
+
+def _note(res: ReplayResult, objs: list[ObjectEstimate]) -> None:
+    for o in objs:
+        res.things[o.oid] = Thing(o.cls, o.first_seen_us, o.last_seen_us, o.x_m, o.y_m, o.sigma_m)
+
+
+def run_replay(folder: Path, cal: Calibration, det: Detector) -> ReplayResult:
+    """Every recorded picture, in order, through the detector and the list (the same answer every time)."""
+    p = ObjectPipeline(cal)
+    res = ReplayResult()
+    for e in replay(folder):
+        if e.text is not None:
+            m = json.loads(e.text)
+            if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
+                p.on_calib(int(m["counts_per_m"]))
+                continue
+            s = parse_td(m)
+            if s is not None:
+                if res.start_us is None:
+                    res.start_us = s.bus_us
+                p.on_telem(s)
+        elif e.frame is not None:
+            rgb = decode(e.frame)
+            if rgb is None:
+                continue
+            cam = p.on_frame(e.frame, (rgb.shape[1], rgb.shape[0]), det.detect(rgb))
+            if not isinstance(cam, str):
+                res.used += 1
+                _note(res, p.objects())
+    res.skips = p.skips()
+    return res
+
+
+def report(res: ReplayResult) -> str:
+    t0 = res.start_us or 0
+    lines = [f"{res.used} pictures used" + (f"; skipped: {res.skips}" if res.skips else "")]
+    if not res.things:
+        lines.append("nothing found")
+    for oid, th in sorted(res.things.items()):
+        lines.append(
+            f"#{oid} {th.cls}: seen {(th.first_us - t0) / 1e6:.1f}-{(th.last_us - t0) / 1e6:.1f} s, "
+            f"last at x {th.x_m * 100:.0f} cm, y {th.y_m * 100:.0f} cm (+-{th.sigma_m * 100:.0f} cm), "
+            f"{math.hypot(th.x_m, th.y_m) * 100:.0f} cm from the start"
+        )
+    return "\n".join(lines)
+
+
+async def run_live(host: str, cal: Calibration, det: Detector) -> None:
+    """The tank: telemetry and the newest picture; the detector in a worker thread (latest picture wins)."""
+    from conqueror_ai.adapters.bridge_ws import BridgeLink
+    from conqueror_ai.adapters.mjpeg_source import MjpegSource
+    from conqueror_ai.adapters.rerun_view import RerunView
+
+    link, cam = BridgeLink(host), MjpegSource(host)
+    view = RerunView(spawn=True)
+    p = ObjectPipeline(cal)
+    stop = asyncio.Event()
+    tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]
+
+    async def telemetry() -> None:
+        calib_given = False
+        while not stop.is_set():
+            s = await link.samples.get()
+            if not calib_given and link.counts_per_m is not None:
+                p.on_calib(link.counts_per_m)
+                calib_given = True
+            p.on_telem(s)
+
+    tasks.append(asyncio.create_task(telemetry()))
+    try:
+        while True:
+            f = await cam.next_frame()
+            rgb = decode(f)
+            if rgb is None:
+                continue
+            dets = await asyncio.to_thread(
+                det.detect, rgb
+            )  # meanwhile newer pictures replace the waiting one
+            used = p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets)
+            view.frame(f, None)
+            view.detections(f, dets)
+            view.plan(f.cap_us, p.pose(), p.objects(), used if isinstance(used, str) else None)
+    finally:
+        stop.set()
+        await link.close()
+        for t in tasks:
+            t.cancel()
+        with contextlib.suppress(asyncio.CancelledError):
+            await asyncio.gather(*tasks, return_exceptions=True)
+
+
+def main() -> None:
+    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
+    ap.add_argument("folder", type=Path, nargs="?")
+    ap.add_argument("--live", action="store_true")
+    ap.add_argument("--host", default=DEFAULT_HOST)
+    ap.add_argument("--calibration", type=Path, default=CALIBRATION)
+    ap.add_argument("--model", type=Path, default=MODEL)
+    a = ap.parse_args()
+    if a.live == (a.folder is not None):
+        raise SystemExit("give a recording folder, or --live")
+    from conqueror_ai.adapters.yolo_coreml import YoloCoreml
+
+    try:
+        cal = calibration_store.load(a.calibration, (320, 240))
+        det = YoloCoreml(a.model)
+    except (FileNotFoundError, ValueError) as err:
+        raise SystemExit(str(err)) from None
+    if a.live:
+        with contextlib.suppress(KeyboardInterrupt):
+            asyncio.run(run_live(a.host, cal, det))
+    else:
+        print(report(run_replay(a.folder, cal, det)))
+
+
+if __name__ == "__main__":
+    main()
```

- [ ] **Step 4: Run all tests, the checks and the tool**

Run: `cd ai-engine && uv sync -q && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright && uv run cq-objects 2>&1 | tail -1`
Expected: `134 passed`; clean; `give a recording folder, or --live`.

- [ ] **Step 5: Commit**

```bash
git add ai-engine/src/conqueror_ai/app/objects.py ai-engine/src/conqueror_ai/adapters/rerun_view.py ai-engine/tests/test_objects_app.py ai-engine/pyproject.toml
git commit -m "feat(ai): cq-objects - the things in the room, live and from a recording (AI engine LLD §15.6)

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 4: On the floor — a ball and a person (owner present)

The tank stands still, or the phone drives slowly. The engine only watches. Ask "stand or floor?" first and say what moves (Step 2 turns the camera, Step 3 nothing).

- [ ] **Step 0:** Set the camera to QVGA (`{"t":"camera_set","size":"QVGA"}`), and back to VGA at the end.
- [ ] **Step 1: Distance.**
  - A ball on tape marks straight ahead at 50 cm, 1 m and 2 m from the lens. The tank stands at the start, camera at the driving tilt (`cam_set 0 -15`).
  - Record each with `cd ai-engine && uv run cq-record --seconds 10 --no-view --out recordings/v-ball-50` (and `-100`, `-200`), then `cd ai-engine && uv run cq-objects recordings/v-ball-50` and so on.
  - Expected: one `sports ball`, "… cm from the start" within 5 cm (50 cm, 1 m) and within 15 cm (2 m) of the tape plus the lens offset (the report measures from the tank's centre, 8.5 cm behind the lens).
  - If YOLO doesn't see the ball as `sports ball`, note what it says and try the owner's other balls.
- [ ] **Step 2: Memory.**
  - The ball at 1 m. Record 30 s with `cq-record`.
  - The camera looks away (`cam_set 90 -15`) for 12 s, then back (`cam_set 0 -15`).
  - Replay with `cq-objects`. Expected: the ball keeps the same `#oid`, seen until it was looked away from and again after.
- [ ] **Step 3: A person** walks across the view at about 1.5 m (record 15 s). Expected: one `person` thing over the walk.
- [ ] **Step 4: Live.**
  - `cd ai-engine && uv run cq-objects --live`: the picture shows boxes, and the plan shows the tank and the things.
  - Ctrl-C ends it cleanly (Review Focus 5).
- [ ] **Step 5:** Write the numbers down in the ledger. Delete the recordings and set the camera back to VGA.

### Task 5: Docs as built

**Files:**
- `docs/lld/ai-engine-lld.md` (§15.9 as built, the floor numbers);
- `docs/design-notes.md` (the results, with how each was measured);
- `CLAUDE.md` (plan list: `2026-10-23-plan-v-objects.md`; Next: phase 5, depth + free floor).

- [ ] **Step 1: Write them.**
- [ ] **Step 2: Commit**

```bash
git add docs CLAUDE.md
git commit -m "docs: Plan V phase 4 - as built and floor results

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
