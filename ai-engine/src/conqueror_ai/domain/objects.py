"""Phase 4: things in the room (AI engine LLD §15, design-notes §134). Pure: detections in, a list out.

- FrameGate: the camera for a picture at its capture time - or why the picture is skipped.
- ObjectLocator: the floor trick - a box's bottom middle is where the thing touches the floor.
- ObjectTracker: one constant-velocity Kalman filter per thing, matched in room positions; Seen / Remembered;
  forgotten after 20 s unseen, or when its spot is in view for 1 s without it.
- ObjectPipeline: telemetry and pictures in order; the detector runs outside (a worker thread)."""

from __future__ import annotations

import math
from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Protocol

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.camera import Calibration, CameraModel, from_servo
from conqueror_ai.domain.pose import PoseFilter, PoseHistory
from conqueror_ai.domain.types import Detection, Frame, ObjectEstimate, Pose, TelemSample

GATE_DPS = 45.0  # turning faster blurs the picture
CAM_MOVING = 1 << 5  # FastFlagBit CAM_MOVING
ANGLES_US = 40_000  # the camera angles must be this close to the capture time
HISTORY_US = 2_000_000
FLOOR_CLASSES = frozenset(
    {
        "person",
        "cat",
        "dog",
        "sports ball",
        "teddy bear",
        "chair",
        "couch",
        "backpack",
        "suitcase",
        "potted plant",
    }
)
CUT_ROWS = (
    6  # a box this near the picture's bottom edge is cut off: a direction only (YOLO stops a few rows short)
)
MAX_RANGE_M = 4.0
GATE_D2 = 9.21  # chi-square, 2 numbers, 99 %
ACCEL_SIGMA = 1.0  # m/s^2: how fast a thing may change its speed
FORGET_S = 20.0
LOOK_S = 1.0
REMEMBER_TAU_S = 1.0  # a remembered thing's speed fades: it has probably stopped
NEW_SPEED_SIGMA = 0.5  # m/s: a new thing's speed is unknown
# Plan V final review:
MAX_REMEMBERED_SIGMA_M = (
    0.5  # a remembered thing's position never gets vaguer than this (it would swallow others)
)
REMEMBERED_WALK_MPS = 0.05  # how far a remembered thing may have been moved, per second, while out of view
REMEMBER_AFTER_S = (
    0.5  # not seen this long (out of view, or no picture used): remembered from its last sighting
)
CONFIRM_HITS = 3  # a new thing is listed once seen this often ...
CONFIRM_S = 0.5  # ... within this long; a new thing missed once is dropped (one false detection is no thing)
LOOK_MISSES = 3  # the look check: at least this many pictures without it, as well as LOOK_S
CAM_RATE_DPS = (
    20.0  # the camera turning faster than this: the picture is skipped (the Uno Lite sends no flag)
)

FloatArray = NDArray[np.float64]


class Detector(Protocol):
    """The detector port (LLD §15.1, §16): YOLO-World and the colour ball finder on the tank's pictures, a
    fake in
    tests. horizon_row: the picture's horizon (the ball finder looks only below it), None when off the
    picture."""

    def detect(self, rgb: NDArray[np.uint8], horizon_row: int | None) -> list[Detection]: ...


def sigma_at(d: float) -> float:
    """The floor trick's expected error at d metres from the camera (Spec 7 §7: ~5 cm at 1 m, ~15 at 2 m)."""
    return 0.02 + 0.03 * d * d


class CameraAngles:
    """The last 2 s of the camera servos (telemetry), for the picture's capture time."""

    def __init__(self) -> None:
        self._s: deque[tuple[int, float, float, int]] = deque()

    def push(self, s: TelemSample) -> None:
        if self._s and s.bus_us < self._s[-1][0]:
            self._s.clear()  # the bridge restarted
        self._s.append((s.bus_us, s.pan_deg, s.tilt_deg, s.flags))
        while s.bus_us - self._s[0][0] > HISTORY_US:
            self._s.popleft()

    def at(self, t_us: int) -> tuple[float, float, int] | None:
        """(pan, tilt, flags) of the nearest sample within 40 ms, or None."""
        best = min(self._s, key=lambda e: abs(e[0] - t_us), default=None)
        if best is None or abs(best[0] - t_us) > ANGLES_US:
            return None
        return best[1], best[2], best[3]

    def rate_dps(self, t_us: int) -> float:
        """How fast the camera turns around t (the faster of pan and tilt, from the samples within 40 ms)."""
        near = [e for e in self._s if abs(e[0] - t_us) <= ANGLES_US]
        if len(near) < 2 or near[-1][0] == near[0][0]:
            return 0.0
        dt = (near[-1][0] - near[0][0]) / 1e6
        return max(abs(near[-1][1] - near[0][1]), abs(near[-1][2] - near[0][2])) / dt


class FrameGate:
    def __init__(self, cal: Calibration, history: PoseHistory, angles: CameraAngles) -> None:
        self.cal = cal
        self._history = history
        self._angles = angles

    def capture_us(self, cap_us: int) -> int:
        """The middle of the picture's readout (LLD §5.4)."""
        return cap_us + self.cal.readout_us // 2

    def camera_for(self, cap_us: int) -> CameraModel | str:
        """The camera model at the capture time, or the reason the picture is skipped."""
        t = self.capture_us(cap_us)
        pose = self._history.at(t)
        if pose is None:
            return "no pose"
        a = self._angles.at(t)
        if a is None:
            return "no camera angles"
        if a[2] & CAM_MOVING or self._angles.rate_dps(t) > CAM_RATE_DPS:
            return "camera moving"
        if abs(math.degrees(pose.w_rps)) > GATE_DPS:
            return "turning"
        return CameraModel(self.cal).for_frame(pose, *from_servo(a[0], a[1]))


@dataclass(frozen=True)
class Located:
    cls: str
    x: float
    y: float
    sigma: float
    box: tuple[float, float, float, float]


@dataclass(frozen=True)
class Near:
    """A thing cut off by the picture's bottom edge: too near for the floor trick, a direction only."""

    cls: str
    bearing_rad: float  # in the room (counter-clockwise from x)
    t_us: int


class ObjectLocator:
    def __init__(self, floor_classes: frozenset[str] = FLOOR_CLASSES) -> None:
        self.floor_classes = floor_classes  # LLD §16: the vocabulary's floor words and the colour balls

    def locate(self, dets: list[Detection], cam: CameraModel, t_us: int) -> tuple[list[Located], list[Near]]:
        located: list[Located] = []
        near: list[Near] = []
        h = cam.cal.size[1]
        c = cam.position()
        for d in dets:
            if d.cls not in self.floor_classes:
                continue
            x1, y1, x2, y2 = d.box
            u = (x1 + x2) / 2.0
            if y2 >= h - CUT_ROWS:
                ray = cam.pixel_ray(u, (y1 + y2) / 2.0)
                if ray is not None:
                    near.append(Near(d.cls, math.atan2(float(ray[1]), float(ray[0])), t_us))
                continue
            p = cam.pixel_to_floor(u, y2)
            if p is None:
                continue
            dist = math.dist((p[0], p[1], 0.0), (float(c[0]), float(c[1]), float(c[2])))
            if dist > MAX_RANGE_M:
                continue
            located.append(Located(d.cls, p[0], p[1], sigma_at(dist), d.box))
        return located, near


_H = np.array([[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0]])


@dataclass
class _Thing:
    oid: int
    cls: str
    x: FloatArray  # x, y, vx, vy
    p: FloatArray  # 4 x 4
    t_us: int
    first_us: int
    last_us: int
    state: str = "seen"
    in_view_since: int | None = None
    misses_in_view: int = 0
    hits: int = 1
    confirmed: bool = False
    x_seen: FloatArray | None = None  # x and p at the last sighting
    p_seen: FloatArray | None = None

    def predict(self, t_us: int) -> None:
        if (
            self.state == "seen"
            and (t_us - self.last_us) / 1e6 > REMEMBER_AFTER_S
            and self.x_seen is not None
        ):
            # not seen for a while (Plan Y Task 5 memory test): from the last sighting it has probably
            # stopped and may have been moved a little - not the walking-person growth of 0.5 m a second,
            # which let a reflection 1.5 m away take the ball
            assert self.p_seen is not None
            self.x, self.p, self.t_us, self.state = (
                self.x_seen.copy(),
                self.p_seen.copy(),
                self.last_us,
                "remembered",
            )
        dt = (t_us - self.t_us) / 1e6
        if dt <= 0:
            return
        f = np.eye(4)
        if self.state == "remembered":  # the speed fades (it has probably stopped), in x and P alike
            e = math.exp(-dt / REMEMBER_TAU_S)
            f[2, 2] = f[3, 3] = e
            f[0, 2] = f[1, 3] = REMEMBER_TAU_S * (1.0 - e)
            q = np.diag([REMEMBERED_WALK_MPS**2 * dt, REMEMBERED_WALK_MPS**2 * dt, 0.0, 0.0])
        else:
            f[0, 2] = f[1, 3] = dt
            g = np.array([[dt * dt / 2, 0.0], [0.0, dt * dt / 2], [dt, 0.0], [0.0, dt]])
            q = g @ g.T * ACCEL_SIGMA**2
        self.x = f @ self.x
        self.p = f @ self.p @ f.T + q
        if self.state == "remembered":
            self._cap_position(MAX_REMEMBERED_SIGMA_M)
        self.t_us = t_us

    def _cap_position(self, sigma_max: float) -> None:
        lam = float(np.linalg.eigvalsh(self.p[:2, :2]).max())
        if lam > sigma_max**2:
            k = math.sqrt(sigma_max**2 / lam)
            self.p[:2, :] *= k
            self.p[:, :2] *= k

    def cost(self, z: Located) -> tuple[float, float]:
        """(d2, d2 + ln det S): the gate uses d2; the ranking also weighs how vague the thing is, so a vague
        remembered thing does not take a detection that fits a sharp one better (Plan V final review I1)."""
        s = _H @ self.p @ _H.T + np.eye(2) * z.sigma**2
        y = np.array([z.x, z.y]) - _H @ self.x
        d2 = float(y @ np.linalg.solve(s, y))
        return d2, d2 + math.log(float(np.linalg.det(s)))

    def update(self, z: Located) -> None:
        s = _H @ self.p @ _H.T + np.eye(2) * z.sigma**2
        k = self.p @ _H.T @ np.linalg.inv(s)
        self.x = self.x + k @ (np.array([z.x, z.y]) - _H @ self.x)
        self.p = (np.eye(4) - k @ _H) @ self.p
        self.x_seen, self.p_seen = self.x.copy(), self.p.copy()

    def estimate(self) -> ObjectEstimate:
        sigma = math.sqrt(float(np.linalg.eigvalsh(self.p[:2, :2]).max()))
        return ObjectEstimate(
            self.oid,
            self.cls,
            float(self.x[0]),
            float(self.x[1]),
            float(self.x[2]),
            float(self.x[3]),
            sigma,
            self.first_us,
            self.last_us,
            self.state,
        )


BALL_PREFIX = "ball:"  # the colour finder's things (§16)
OTHER_WORD_RANK = 1e9  # a word box on a ball is tried after every pair of the same class


def _is_ball(cls: str) -> bool:
    return cls.startswith(BALL_PREFIX)


@dataclass
class ObjectTracker:
    """One thing per thing. A colour ball is one thing whatever YOLO-World calls it (Plan Y final review I2):
    a word box (say "toy") at a listed ball's place updates that ball when nothing of its own class takes it
    (the finder missed the ball: at the picture's edge, half hidden); and when the finder sees a listed ball,
    a word thing at its place not seen in that picture is the ball (its boxes were dropped as the ball's)
    and goes."""

    _things: list[_Thing] = field(default_factory=lambda: list[_Thing]())
    _next: int = 1

    def update(self, located: list[Located], cam: CameraModel, t_us: int) -> None:
        for th in self._things:
            th.predict(t_us)
        pairs: list[tuple[float, int, int]] = []
        for i, th in enumerate(self._things):
            for j, z in enumerate(located):
                same = th.cls == z.cls
                if same or (th.confirmed and _is_ball(th.cls) and not _is_ball(z.cls)):
                    d2, rank = th.cost(z)
                    if d2 <= GATE_D2:
                        pairs.append((rank if same else rank + OTHER_WORD_RANK, i, j))
        pairs.sort()
        used_t: set[int] = set()
        used_z: set[int] = set()
        balls_seen: list[Located] = []  # the finder's balls matched to listed balls in this picture
        for _, i, j in pairs:
            if i in used_t or j in used_z:
                continue
            used_t.add(i)
            used_z.add(j)
            if self._things[i].confirmed and _is_ball(located[j].cls):
                balls_seen.append(located[j])
            th = self._things[i]
            th.update(located[j])
            th.state, th.last_us, th.in_view_since, th.misses_in_view = "seen", t_us, None, 0
            th.hits += 1
            if th.hits >= CONFIRM_HITS:
                th.confirmed = True
        keep: list[_Thing] = []
        for i, th in enumerate(self._things):
            if i not in used_t and not _is_ball(th.cls) and any(th.cost(z)[0] <= GATE_D2 for z in balls_seen):
                continue  # a word thing where the finder sees a ball: it was that ball
            if not th.confirmed and (i not in used_t or (t_us - th.first_us) / 1e6 > CONFIRM_S):
                continue  # a new thing missed once, or not confirmed in time: one false detection is no thing
            if i not in used_t and not self._still_there(th, cam, t_us):
                continue
            keep.append(th)
        for j, z in enumerate(located):
            if j not in used_z:
                p = np.diag([z.sigma**2, z.sigma**2, NEW_SPEED_SIGMA**2, NEW_SPEED_SIGMA**2])
                x = np.array([z.x, z.y, 0.0, 0.0])
                keep.append(
                    _Thing(self._next, z.cls, x, p, t_us, t_us, t_us, x_seen=x.copy(), p_seen=p.copy())
                )
                self._next += 1
        self._things = keep

    @staticmethod
    def _still_there(th: _Thing, cam: CameraModel, t_us: int) -> bool:
        """An unmatched thing: kept unless forgotten, or its spot has been in view for 1 s without it."""
        if (t_us - th.last_us) / 1e6 >= FORGET_S:
            return False
        if cam.sees(float(th.x[0]), float(th.x[1]), MAX_RANGE_M):  # the whole tracked range (review I3)
            if th.in_view_since is None:
                th.in_view_since = t_us
            th.misses_in_view += 1
            return th.misses_in_view < LOOK_MISSES or (t_us - th.in_view_since) / 1e6 < LOOK_S
        th.state, th.in_view_since, th.misses_in_view = "remembered", None, 0
        return True

    def objects(self) -> list[ObjectEstimate]:
        """The confirmed things (new ones are listed once seen CONFIRM_HITS times)."""
        return [th.estimate() for th in self._things if th.confirmed]


class ObjectPipeline:
    """Telemetry and pictures in arrival order -> the list. The detector is run outside and handed in."""

    def __init__(self, cal: Calibration, floor_classes: frozenset[str] = FLOOR_CLASSES) -> None:
        self.cal = cal
        self._pose = PoseFilter()
        self._history = PoseHistory()
        self._angles = CameraAngles()
        self.gate = FrameGate(cal, self._history, self._angles)
        self._locator = ObjectLocator(floor_classes)
        self.tracker = ObjectTracker()
        self._near: list[Near] = []
        self._skips: Counter[str] = Counter()
        self.last_camera: CameraModel | None = None
        self._last_cap_us: int | None = None

    def on_calib(self, counts_per_m: int) -> None:
        self._pose.set_counts_per_m(counts_per_m)

    def on_telem(self, s: TelemSample) -> None:
        self._history.push(self._pose.update(s))
        self._angles.push(s)

    def camera_at(self, cap_us: int) -> CameraModel | str:
        """The camera for a picture before detecting (for its horizon), or why the gate skips it. Hand it to
        on_frame, so the horizon and the map use the one camera (Plan Y final review m8)."""
        return self.gate.camera_for(cap_us)

    def on_frame(
        self, f: Frame, size: tuple[int, int], dets: list[Detection], cam: CameraModel | str | None = None
    ) -> CameraModel | str:
        """Use one picture (its decoded size, its detections, the camera from camera_at if taken): its camera,
        or why it was skipped."""
        if self._last_cap_us is not None and f.cap_us < self._last_cap_us:
            self.tracker = (
                ObjectTracker()
            )  # the bridge restarted: the old list's times are from another clock
        self._last_cap_us = f.cap_us
        if size != self.cal.size:
            self._skips["picture size"] += 1
            return "picture size"
        if cam is None:
            cam = self.gate.camera_for(f.cap_us)
        if isinstance(cam, str):
            self._skips[cam] += 1
            return cam
        t = self.gate.capture_us(f.cap_us)
        located, self._near = self._locator.locate(dets, cam, t)
        self.tracker.update(located, cam, t)
        self.last_camera = cam
        return cam

    def objects(self) -> list[ObjectEstimate]:
        return self.tracker.objects()

    def near(self) -> list[Near]:
        return list(self._near)

    def skips(self) -> dict[str, int]:
        return dict(self._skips)

    def pose(self) -> Pose | None:
        return self._pose.pose()
