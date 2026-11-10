"""The run's pure state (AI engine LLD §19.5, review F1): telemetry and pictures in, a SkillInput out. No I/O,
no clock - the time is handed in. MapRun (moved here from app/map.py) keeps the pose, the room map and the
objects; World adds the goal, the memory of things, the play area and the per-picture flags."""

from __future__ import annotations

import json
from collections import deque
from collections.abc import Callable, Sequence

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.app.objects import DepthFn, depth_due, see
from conqueror_ai.domain.camera import Calibration, CameraModel, FloatArray, from_servo
from conqueror_ai.domain.depth import MapConfig, Seeing, Seen
from conqueror_ai.domain.drive.goal import Goal, size_positions
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.skill import SkillInput, make_input
from conqueror_ai.domain.memory import ThingMemory
from conqueror_ai.domain.objects import Glimpse, Located, ObjectPipeline
from conqueror_ai.domain.person import PersonWatch
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.sonar_model import SonarModel
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Detection, Frame, TelemSample

SIGHTINGS_KEEP_US = 500_000  # SEE? / HOWFAR count a single sighting this long (AI engine LLD §25.9)


def maybes_of(maybes: Sequence[Located], kind: Callable[[str], bool]) -> list[tuple[float, float]]:
    """The newest picture's sightings of the thing sought, sure or not: find's candidates (§179, §189)."""
    return [(m.x, m.y) for m in maybes if kind(m.cls)]


class MapRun:
    """Telemetry and pictures in arrival order -> the room map (the depth model handed in)."""

    def __init__(
        self,
        cal: Calibration,
        cfg: MapConfig,
        depth: DepthFn | None,
        p: ObjectPipeline | None = None,
        lim: Limits | None = None,
        room: RoomMap | None = None,
    ) -> None:
        """p: cq-drive's pipeline with its detectors (Plan Z5); none: the pose and pictures' cameras only.
        lim: the tank's body for the floor under it (Plan Z6); the default body when none. room: a map handed
        in (Plan Z8 loads one); none: a new one."""
        self.cfg = cfg
        self.lim = lim or Limits()
        self.depth = depth
        self.p = p or ObjectPipeline(cal, frozenset())
        self.room = room if room is not None else RoomMap(cfg.map)
        self.sonar = SonarModel(max_m=cfg.ranges.sonar_max_m)
        self.seeing = Seeing(cfg)
        self.angle_trusted = True
        self.used = 0
        self.depth_pictures = 0
        self.marked = 0
        self.echoes = 0
        self.pan_servo_deg: float | None = None  # the newest telemetry's pan servo (the ROOM tab's view)

    def on_calib(self, counts_per_m: int) -> None:
        self.p.on_calib(counts_per_m)

    def camera_pan_left_deg(self) -> float | None:
        """The camera's real pan from straight ahead, left + like the heading (the ROOM tab's camera view, LLD
        §23.2): the servo through the calibration's pan_map; None before any telemetry."""
        if self.pan_servo_deg is None:
            return None
        g, o = self.p.cal.pan_map
        right, _ = from_servo(self.pan_servo_deg, 90.0)
        return -(g * right + o)

    def on_telem(self, s: TelemSample) -> None:
        self.p.on_telem(s)
        self.pan_servo_deg = s.pan_deg
        pose = self.p.pose()
        dist = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0
        if pose is not None and self.sonar.update(self.room, pose, dist, s.bus_us):
            self.echoes += 1
        if pose is not None:  # the floor under the tank is free (Plan Z6)
            lim = self.lim
            self.room.body_over(pose, lim.body_front_m, lim.body_rear_m, lim.body_half_w_m, s.bus_us)

    def on_text(self, text: str) -> None:
        """One recorded message: the tank's calibration or a telemetry sample."""
        m = json.loads(text)
        if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
            self.on_calib(int(m["counts_per_m"]))
            return
        s = parse_td(m)
        if s is not None:
            self.on_telem(s)

    def gate(
        self,
        f: Frame,
        rgb: NDArray[np.uint8],
        dets: list[Detection] | None = None,
        cam: CameraModel | str | None = None,
    ) -> CameraModel | str:
        """The picture (with its detections, and its camera if taken before detecting) into the pipeline: its
        camera when the map uses it, else why it was skipped ("turning", "camera moving", ...)."""
        used = self.p.on_frame(f, (rgb.shape[1], rgb.shape[0]), dets or [], cam or self.p.camera_at(f.cap_us))
        if not isinstance(used, str):
            self.used += 1
        return used

    def depth_due(self) -> bool:
        return self.depth is not None and depth_due(self.used, self.cfg.depth.every_n)

    def depth_due_next(self) -> bool:
        """Depth for the next picture if the map uses it (asked before detecting, as cq-drive does)."""
        return self.depth is not None and depth_due(self.used + 1, self.cfg.depth.every_n)

    def apply(self, seen: Seen, t_us: int) -> None:
        """A seen picture onto the map."""
        self.depth_pictures += 1
        self.angle_trusted = seen.fix.trusted
        if seen.marks is not None:
            pose = self.p.pose()
            self.room.update_picture(seen.marks, t_us, None if pose is None else (pose.x_m, pose.y_m))
            self.marked += 1

    def on_picture(self, f: Frame, rgb: NDArray[np.uint8]) -> Seen | None:
        """One picture, all here (the replay)."""
        cam = self.gate(f, rgb)
        if isinstance(cam, str) or self.depth is None or not self.depth_due():
            return None
        seen = see(self.seeing, self.p, cam, self.depth(rgb), f.cap_us)
        self.apply(seen, f.cap_us)
        return seen

    def snapshot(self, t_us: int, stale: bool) -> MapSnapshot:
        return self.room.snapshot(t_us, self.p.pose(), stale, self.angle_trusted)


class World:
    """One run's state for a skill: MapRun (pose, map, objects, seeing), the goal, the memory, the play area,
    the newest echo and the per-picture flags."""

    def __init__(
        self,
        cal: Calibration,
        cfg: MapConfig,
        depth: DepthFn | None,
        floor: frozenset[str],
        sizes: dict[str, float],
        lim: Limits,
        kind: Callable[[str], bool] | None,
        *,
        room: RoomMap | None = None,
        memory: ThingMemory | None = None,
        area_m: float | None = None,
        run_id: str = "",
    ) -> None:
        """kind: the thing a find seeks (None: no goal - wander, explore, cq-map). room, memory: handed in
        (Plan Z8 loads them). area_m: the play area round the first pose."""
        self.map_run = MapRun(cal, cfg, depth, p=ObjectPipeline(cal, floor), lim=lim, room=room)
        self.memory = memory if memory is not None else ThingMemory()
        self.goal = None if kind is None else Goal(kind)
        self.cfg, self.lim, self.sizes = cfg, lim, sizes
        self.area_m, self.run_id = area_m, run_id
        self.area: tuple[float, float, float] | None = None
        self.sonar_m: float | None = None
        # the person boxes come here and only here (LLD §28.1): the tracker, memory and map refuse them
        self.person = PersonWatch()
        self._person_us: int | None = None  # the newest used picture's time
        self.last_frame: Frame | None = None
        self._by_size: dict[str, tuple[float, float]] = {}
        self._cam_still = True
        self._used = False  # a picture was used since the last input
        self._fresh = False  # ... or gave find maybes (used, or taken while turning: LLD §25 A1)
        # the goal's glimpses since the last input (§25 A2); every sighting of the last 0.5 s (§25.9)
        self._glimpses: list[Glimpse] = []
        self._sightings: deque[tuple[int, list[Located]]] = deque()

    @classmethod
    def around(cls, run: MapRun) -> World:
        """A World with no goal round a MapRun made elsewhere (cq-map's live view hands one in)."""
        w = cls(run.p.cal, run.cfg, run.depth, frozenset(), {}, run.lim, None)
        w.map_run = run
        return w

    def set_floor_classes(self, floor: frozenset[str]) -> None:
        """The classes the floor trick places, after a word is taught (§22.4)."""
        self.map_run.p.set_floor_classes(floor)

    # ---- in
    def on_calib(self, counts_per_m: int) -> None:
        self.map_run.on_calib(counts_per_m)

    def on_telem(self, s: TelemSample) -> None:
        self.map_run.on_telem(s)
        self.sonar_m = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0

    def on_text(self, text: str) -> None:
        """One recorded message (a replay): the calibration or a telemetry sample."""
        m = json.loads(text)
        if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
            self.on_calib(int(m["counts_per_m"]))
            return
        s = parse_td(m)
        if s is not None:
            self.on_telem(s)

    def on_picture(
        self,
        f: Frame,
        rgb: NDArray[np.uint8],
        dets: list[Detection],
        cam: CameraModel | str | None = None,
        inv: FloatArray | None = None,
        with_depth: bool = False,
    ) -> CameraModel | str:
        """A picture and its detections (and its camera if taken before detecting): the camera when the map
        used it, else why it was skipped. Seeing here when `inv` is given, or with `with_depth` when depth is
        due (a replay); live, Perception sees off the event loop (`see`, then `apply`)."""
        used = self.map_run.gate(f, rgb, dets, cam)
        self._fresh = self._fresh or self.map_run.p.fresh()
        t = self.map_run.p.gate.capture_us(
            f.cap_us
        )  # the readout's middle, as the tracker's times (review m3)
        if self._sightings and t < self._sightings[-1][0]:
            self._sightings.clear()  # the bridge restarted: the old times are from another clock (review m1)
        if self.map_run.p.fresh():
            self._sightings.append((t, list(self.map_run.p.maybes())))
        while self._sightings and t - self._sightings[0][0] > SIGHTINGS_KEEP_US:
            self._sightings.popleft()
        if self.goal is not None:
            self._glimpses += [g for g in self.map_run.p.glimpses() if self.goal.kind(g.cls)]
        self.last_frame = f
        self._cam_still = used != "camera moving"
        if isinstance(used, str):
            self._by_size = {}
            if used == "turning":  # owner, design-notes §229: the gyro places the person; the map skips it
                rough = self.map_run.p.gate.camera_turning(f.cap_us)
                if rough is not None:
                    self.person.update([d.box for d in dets if d.cls == "person"], rough, self.sonar_m, t)
                    self._person_us = t
            return used
        self._used = True
        self.person.update([d.box for d in dets if d.cls == "person"], used, self.sonar_m, t)
        self._person_us = t
        self._by_size = size_positions(dets, self.sizes, used)
        depth = self.map_run.depth
        if inv is None and with_depth and depth is not None and self.map_run.depth_due():
            inv = depth(rgb)
        if inv is not None:
            self.apply(self.see(used, inv, f.cap_us), f.cap_us)
        return used

    def recent_sightings(self, now_us: int) -> list[Located]:
        """Every class's single sightings captured in the 0.5 s before `now_us` (SEE? / HOWFAR, §25.9)."""
        return [s for t, ss in self._sightings if 0 <= now_us - t <= SIGHTINGS_KEEP_US for s in ss]

    def see(self, cam: CameraModel, inv: FloatArray, t_us: int) -> Seen:
        """The depth picture through Seeing (pure: safe off the event loop)."""
        return see(self.map_run.seeing, self.map_run.p, cam, inv, t_us)

    def apply(self, seen: Seen, t_us: int) -> None:
        self.map_run.apply(seen, t_us)

    # ---- out
    def input(self, now_s: float, wall_s: float, map_ok: bool) -> SkillInput:
        """What the skill gets this tick (LLD §19.4): the pose, the snapshot and its cost maps (the play area
        set at the first pose), the goal's fix, the memory updated, the maybes of the kind, the flags."""
        p = self.map_run.p
        pose = p.pose()
        objects = p.objects()
        self.memory.update(objects, wall_s, self.run_id)
        used, self._used = self._used, False
        fresh, self._fresh = self._fresh, False
        glimpses, self._glimpses = tuple(self._glimpses), []
        now_us = pose.t_us if pose is not None else self._person_us
        person = None if now_us is None else self.person.latest(max(now_us, self._person_us or 0))
        if pose is None:
            return make_input(
                at=None,
                snap=None,
                map_ok=map_ok,
                sonar_m=self.sonar_m,
                used=used,
                fresh=fresh,
                glimpses=glimpses,
                person=person,
            )
        if self.area_m is not None and self.area is None:
            self.area = (pose.x_m, pose.y_m, self.area_m)
        fix = (
            None
            if self.goal is None
            else self.goal.update(objects, (pose.x_m, pose.y_m), pose.t_us, self._by_size)
        )
        maybes = () if self.goal is None else maybes_of(p.maybes(), self.goal.kind)
        snap: MapSnapshot = self.map_run.snapshot(pose.t_us, not map_ok)
        return make_input(
            at=(pose.x_m, pose.y_m, pose.th_rad),
            snap=snap,
            map_ok=map_ok,
            area=self.area,
            settings=self.cfg.cost,
            fix=fix,
            sonar_m=self.sonar_m,
            cam_still=self._cam_still,
            used=used,
            maybes=maybes,
            fresh=fresh,
            glimpses=glimpses,
            person=person,
        )
