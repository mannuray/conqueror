"""The game skills (AI engine LLD §28.3, Spec 10): FACE, ESCAPE and FOLLOW behind the Z7 Skill interface.

The person is kept as a floor point made from each new estimate (PersonEst: from the front edge, pessimistic),
so a turn knows where they are while the camera's pictures are skipped (the gate drops pictures taken spinning
faster than GATE_DPS). Unseen time counts only while the tank is not spinning: a spin is not "lost". The
bridge's keep-out, game cap and caught sit under every command (bridge LLD R34)."""

from __future__ import annotations

import math
from dataclasses import replace

from conqueror_ai.domain.drive.curves import STOP, Command, XYTh, ramp
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.planner import Planner
from conqueror_ai.domain.drive.roam import POINT_DONE_M, RoamSkill
from conqueror_ai.domain.drive.route import XY, RoutePlanner
from conqueror_ai.domain.drive.skill import Result, SkillInput, SkillOut
from conqueror_ai.domain.drive.trusted import TrustedFloor
from conqueror_ai.domain.objects import GATE_DPS

FACE_PAN_DEG = 35.0  # the camera's comfortable pan; beyond it FACE turns the tank (LLD §28.3)
FACE_DONE_DEG = 10.0  # turned this near the person's direction: facing them
TURN_FROM_M = 0.35  # a turn on the spot only with the person this far or farther (bridge LLD R34.2)
LOST_S = 1.0  # not seen this long (while not spinning): lost
VIEW_DEG = 41.0  # half the camera's view: ESCAPE knows where the person is only inside it
ESCAPE_NEAR_M = 0.70  # nearer, ESCAPE never turns its back (S11): back over trusted floor, or stay
FOLLOW_MIN_M = 0.50  # FOLLOW never nearer than this, whatever the setting (S4)
FOLLOW_PAN_DEG = 60.0  # FOLLOW keeps the camera on the person up to this pan
PAN_STEP_DEG = 3.0  # the camera is sent again only when it should move more than this
TURN_PAN_STEP_DEG = 20.0  # ESCAPE's camera in steps this big (a moving camera's pictures are skipped)
ESCAPE_PAN_DEG = 90.0  # driving sideways away the person drifts behind the side: the servo's whole pan
SONAR_SPARE_M = 0.10  # a side is open when the sonar's echo is beyond the leg + this (+ the front)
AWAY_MAX_S = 10.0  # the sideways leg ends no_way after this (it turns back to face them first)
REPLAN_M = 0.15  # FOLLOW's goal moved this far: a new route
GAME_V_CAP_CMS = 25.0  # a game drives at most this fast (LLD §28.1)
FRESH_US = 300_000  # ESCAPE starts only on an estimate this recent (the bridge's deadman; final review M1)


def _wrap(deg: float) -> float:
    return (deg + 180.0) % 360.0 - 180.0


def game_limits(lim: Limits) -> Limits:
    return replace(lim, v_cap_cms=min(lim.v_cap_cms, GAME_V_CAP_CMS))


class Sight:
    """The person as a floor point from each new estimate, and how long unseen while the tank could see."""

    def __init__(self, lim: Limits) -> None:
        self.lim = lim
        self.xy: XY | None = None
        self.t_us: int | None = None
        self.unseen_s = 0.0

    def update(self, inp: SkillInput, dt: float, spinning: bool) -> None:
        p, at = inp.person, inp.at
        if p is not None and p.seen and p.d_m is not None and at is not None and p.t_us != self.t_us:
            self.t_us, self.unseen_s = p.t_us, 0.0
            fx, fy = self.front(at)
            a = at[2] + math.radians(p.bearing_deg)
            self.xy = (fx + p.d_m * math.cos(a), fy + p.d_m * math.sin(a))
        elif not spinning:
            self.unseen_s += dt

    @property
    def lost(self) -> bool:
        return self.unseen_s >= LOST_S - 1e-9

    def front(self, at: XYTh) -> XY:
        return at[0] + self.lim.body_front_m * math.cos(at[2]), at[1] + self.lim.body_front_m * math.sin(
            at[2]
        )

    def bearing(self, at: XYTh) -> float:
        """From the front edge's middle, + left (deg); 0 with no point."""
        if self.xy is None:
            return 0.0
        fx, fy = self.front(at)
        return _wrap(math.degrees(math.atan2(self.xy[1] - fy, self.xy[0] - fx) - at[2]))

    def dist(self, at: XYTh) -> float:
        return math.inf if self.xy is None else math.dist(self.front(at), self.xy)

    def heading(self, at: XYTh) -> float:
        """The heading (rad) from the tank's centre to the person."""
        assert self.xy is not None
        return math.atan2(self.xy[1] - at[1], self.xy[0] - at[0])


class _Base:
    """The ramp, the clock, the endings and a spin to a heading, shared by the three skills."""

    def __init__(self, lim: Limits) -> None:
        self.lim = game_limits(lim)
        self.sight = Sight(self.lim)
        self.cmd = STOP
        self.state = "start"
        self._last_s: float | None = None
        self._pan: float | None = None

    def _clock(self, now_s: float, inp: SkillInput) -> None:
        dt = self.lim.dt_s if self._last_s is None else max(0.0, now_s - self._last_s)
        self._last_s = now_s
        self.sight.update(inp, dt, abs(self.cmd.w_dps) > GATE_DPS)

    def _out(self, want: Command, cam: tuple[float, float] | None = None) -> SkillOut:
        self.cmd = ramp(self.cmd, want, self.lim.dt_s, self.lim)
        return SkillOut(self.cmd, cam)

    def _halt(self, cam: tuple[float, float] | None = None) -> SkillOut:
        self.cmd = STOP
        return SkillOut(STOP, cam)

    def _end(self, result: Result, detail: str = "", cam: tuple[float, float] | None = None) -> SkillOut:
        self.state = "done" if result == Result.DONE else "failed"
        self.cmd = STOP
        return SkillOut(STOP, cam, result, detail)

    def _cam_to(
        self, at: XYTh, limit_deg: float, step_deg: float = PAN_STEP_DEG
    ) -> tuple[float, float] | None:
        """The camera on the person (pan + = right), sent when it should move more than `step_deg`."""
        pan = max(-limit_deg, min(limit_deg, -self.sight.bearing(at)))
        if self._pan is not None and abs(pan - self._pan) <= step_deg:
            return None
        self._pan = pan
        return (pan, self.lim.drive_tilt_deg)

    def _spin_to(self, at: XYTh, th: float) -> SkillOut | None:
        """A turn on the spot toward heading `th`; None when within FACE_DONE_DEG (the tank stops)."""
        err = _wrap(math.degrees(th - at[2]))
        if abs(err) <= FACE_DONE_DEG:
            self.cmd = STOP
            return None
        return self._out(Command(0.0, math.copysign(self.lim.spin_dps, err)))

    def debug(self) -> dict[str, object]:
        return {"state": self.state, "person": self.sight.xy, "unseen_s": round(self.sight.unseen_s, 2)}

    def cancel(self) -> SkillOut:
        return self._end(Result.CANCELLED)


class FaceSkill(_Base):
    """FACE "baby: the camera pans to the person within ±35°; beyond, the tank turns on the spot (from 35 cm
    only) until within 10°. done facing them, lost when not seen for 1 s."""

    def __init__(self, lim: Limits) -> None:
        super().__init__(lim)
        self._th: float | None = None

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self._clock(now_s, inp)
        at = inp.at
        if at is None or self.sight.xy is None:
            return self._end(Result.LOST, "the person is not in view") if self.sight.lost else self._halt()
        if self._th is not None:  # turning: by the odometry, toward where they were
            out = self._spin_to(at, self._th)
            return out if out is not None else self._end(Result.DONE, cam=self._cam_to(at, FACE_PAN_DEG))
        if self.sight.lost:
            return self._end(Result.LOST, "the person is not in view")
        b = self.sight.bearing(at)
        if abs(b) <= FACE_PAN_DEG or self.sight.dist(at) < TURN_FROM_M:
            return self._end(Result.DONE, cam=self._cam_to(at, FACE_PAN_DEG))
        self._th, self.state = self.sight.heading(at), "turn"
        return self._spin_to(at, self._th) or self._halt()


class EscapeSkill(_Base):
    """ESCAPE "baby cm: the person ahead in view. The floor behind trusted: back `back_m` straight facing
    them. Not trusted and the person at least ESCAPE_NEAR_M away: turn ~90 deg to a side with a route (left
    first), drive `back_m` sideways away with the camera on them, turn back to face them (owner, design-notes
    §229: the person never behind the camera). Else stay still, no_way (S11: never the back to a person)."""

    def __init__(self, lim: Limits, back_m: float, trusted: TrustedFloor | None = None) -> None:
        super().__init__(lim)
        self.back_m = back_m
        self.trusted = trusted or TrustedFloor()
        self._from: XYTh | None = None
        self._th: float | None = None
        self._away: RoamSkill | None = None
        self._spot: XY | None = None
        self._sides: list[tuple[float, XY]] = []  # the sides still to try: heading and spot
        self._away_t0 = 0.0
        self._failed = ""  # why the away leg failed: no_way once facing them again

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self._clock(now_s, inp)
        at = inp.at
        if at is None:
            return self._halt()
        if self.state == "start":
            return self._start(at, inp)
        if self.state == "back":
            return self._backing(at)
        cam = self._cam_to(at, ESCAPE_PAN_DEG, TURN_PAN_STEP_DEG)  # the person in view all the while
        if self.state in ("turn_away", "turn_back"):
            assert self._th is not None
            out = self._spin_to(at, self._th)
            if out is not None:
                return SkillOut(out.cmd, cam)
            if self.state == "turn_back":
                result = Result.NO_WAY if self._failed else Result.DONE
                return self._end(result, self._failed, cam=self._cam_to(at, FACE_PAN_DEG))
            if not self._open(inp):  # the sonar, now pointing along the leg: something there
                return self._next_side(at, cam)
            assert self._spot is not None
            self._away, self.state = RoamSkill(Planner(self.lim), "point", point=self._spot), "away"
            self._away_t0 = now_s
            return self._halt(cam)
        assert self._away is not None
        out = self._away.tick(now_s, inp)
        late = now_s - self._away_t0 >= AWAY_MAX_S
        if out.result is None and not late:
            self.cmd = out.cmd
            return SkillOut(out.cmd, cam)
        if late or out.result != Result.DONE:
            why = "too long" if out.result is None else f"{out.result.value} {out.detail}".strip()
            self._failed = f"no way away: {why}"
        self._th, self.state = self.sight.heading(at), "turn_back"  # facing them again either way
        return self._halt(cam)

    def _backing(self, at: XYTh) -> SkillOut:
        """Straight back at v_min until the odometry moved back_m along the start heading."""
        assert self._from is not None
        x0, y0, th0 = self._from
        moved = -((at[0] - x0) * math.cos(th0) + (at[1] - y0) * math.sin(th0))
        return self._end(Result.DONE) if moved >= self.back_m - 1e-6 else self._out(Command(-1.0, 0.0))

    def _start(self, at: XYTh, inp: SkillInput) -> SkillOut:
        p, snap = inp.person, inp.snap
        if p is None or not p.seen or p.d_m is None or abs(p.bearing_deg) > VIEW_DEG:
            return self._end(Result.NO_WAY, "the person is not in view")  # never backs toward the unknown
        if snap is None or snap.t_us - p.t_us > FRESH_US:
            return self._end(Result.NO_WAY, "no fresh picture of the person")
        if self.trusted.ok(snap, at, self.back_m, snap.t_us, lim=self.lim):
            self._from, self.state = at, "back"
            return self._out(Command(-1.0, 0.0))
        if p.d_m < ESCAPE_NEAR_M:
            return self._end(Result.NO_WAY, "the person is near and the floor behind is not trusted")
        self._sides = self._routed(at, inp)
        return self._next_side(at, self._cam_to(at, ESCAPE_PAN_DEG, TURN_PAN_STEP_DEG))

    def _routed(self, at: XYTh, inp: SkillInput) -> list[tuple[float, XY]]:
        """Left, then right: the heading 90 deg round and the spot back_m + 15 cm along it, where the cost map
        has a route. Beside the tank is never in view (it keeps the person in view): the sonar decides after
        the turn."""
        if inp.costs is None:
            return []
        cost, routes, far = inp.costs.base(), RoutePlanner(self.lim), self.back_m + POINT_DONE_M
        out: list[tuple[float, XY]] = []
        for turn in (math.pi / 2, -math.pi / 2):
            th = at[2] + turn
            spot = (at[0] + far * math.cos(th), at[1] + far * math.sin(th))
            if routes.route(cost, (at[0], at[1]), spot) is not None:
                out.append((th, spot))
        return out

    def _next_side(self, at: XYTh, cam: tuple[float, float] | None) -> SkillOut:
        """Turn to the next side to try (through facing them, never the back); none left: face them,
        no_way."""
        if not self._sides:
            self._failed = "no way to either side"
            self._th, self.state = self.sight.heading(at), "turn_back"
            return self._halt(cam)
        self._th, self._spot = self._sides.pop(0)
        self.state = "turn_away"
        return self._halt(cam)

    def _open(self, inp: SkillInput) -> bool:
        s = inp.sonar_m
        return s is None or s > self.back_m + POINT_DONE_M + SONAR_SPARE_M


class FollowSkill(_Base):
    """FOLLOW "baby s: for `seconds`, a route to a spot the follow distance short of the person on the tank's
    side (never a route to them, S4; never nearer than FOLLOW_MIN_M); stops inside it and keeps facing them.
    done at `seconds`, lost when not seen for 1 s."""

    def __init__(self, lim: Limits, seconds: float, stop_m: float = 1.0) -> None:
        super().__init__(lim)
        self.seconds = seconds
        self.stop_m = max(FOLLOW_MIN_M, stop_m)
        self._t0: float | None = None
        self._route: RoamSkill | None = None
        self._goal: XY | None = None

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self._clock(now_s, inp)
        if self._t0 is None:
            self._t0 = now_s
        if now_s - self._t0 >= self.seconds - 1e-9:
            return self._end(Result.DONE)
        if self.sight.lost:
            return self._end(Result.LOST, "the person is not in view")
        at = inp.at
        if at is None or self.sight.xy is None:
            return self._halt()
        cam = self._cam_to(at, FOLLOW_PAN_DEG)
        if self.sight.dist(at) <= self.stop_m:
            self._route, self.state = None, "near"
            return self._halt(cam)
        goal = self._goal_for(at)
        if self._route is None or self._goal is None or math.dist(goal, self._goal) > REPLAN_M:
            self._route, self._goal = RoamSkill(Planner(self.lim), "point", point=goal), goal
        self.state = "follow"
        out = self._route.tick(now_s, inp)
        if out.result is not None:  # there, or no way for now: wait facing them and plan again
            self._route = None
            return self._halt(cam)
        self.cmd = out.cmd
        return SkillOut(out.cmd, cam)

    def _goal_for(self, at: XYTh) -> XY:
        """The tank's centre stop_m + its front short of the person, on the line toward the tank."""
        assert self.sight.xy is not None
        px, py = self.sight.xy
        d = math.dist((px, py), at[:2])
        k = min(1.0, (self.stop_m + self.lim.body_front_m) / max(d, 1e-6))
        return px + (at[0] - px) * k, py + (at[1] - py) * k
