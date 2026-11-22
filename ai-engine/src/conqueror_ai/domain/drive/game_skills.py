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
from conqueror_ai.domain.drive.roam import RoamSkill
from conqueror_ai.domain.drive.route import XY, RoutePlanner
from conqueror_ai.domain.drive.skill import Result, SkillInput, SkillOut
from conqueror_ai.domain.objects import GATE_DPS
from conqueror_ai.domain.person import PersonEst

FACE_PAN_DEG = 35.0  # the camera's comfortable pan; beyond it FACE turns the tank (LLD §28.3)
FACE_DONE_DEG = 10.0  # turned this near the person's direction: facing them
FACE_LOOKS = 3  # FACE turns at most this many times on side-edge sightings, then ends done (LLD §28.12)
FACE_LOOK_FROM_DEG = 30.0  # nobody in view, last seen beyond this: the camera looks that way first (§28.13)
FACE_LOOK_PAN_DEG = 60.0  # ... this far at most (the camera then covers ~100 deg to that side)
SWEEP_DWELL_S = 0.6  # nobody in view: the camera looks three ways, each this long (in the bridge's 3 s, §248)
BLIND_TURN_M = 0.30  # ... not found: the tank turns to look only when last seen this far (bridge R34.14)
FACE_STEP_LOOK_S = 1.0  # ... in TURN_STEP_DEG steps, looking this long after each, a full circle at most
TURN_FROM_M = 0.15  # a turn on the spot only with the person this far or farther (bridge LLD R34.14)
LOST_S = 1.0  # not seen this long (while not spinning): lost
VIEW_DEG = 41.0  # half the camera's view: ESCAPE knows where the person is only inside it
FACE_MIN_S = 0.5  # FACE is done no sooner: a program looping on it runs ~2 a second (design-notes §251)
FOLLOW_MIN_M = 0.30  # FOLLOW never nearer than this, whatever the setting (S4, §248)
FOLLOW_SPIN_FROM_DEG = (
    90.0  # FOLLOW curves up to this far off; behind it, a turn on the spot (design-notes §250)
)
FOLLOW_PAN_DEG = 60.0  # FOLLOW keeps the camera on the person up to this pan
PAN_STEP_DEG = 3.0  # the camera is sent again only when it should move more than this
TURN_STEP_DEG = 30.0  # FACE's blind steps this big (design-notes §241, §248)
ESCAPE_PAN_DEG = 90.0  # driving sideways away the person drifts behind the side: the servo's whole pan
BACK_OFF_M = 0.30  # ESCAPE backs off this far at once, over any floor (owner, design-notes §254)
WALL_S = 0.5  # ... a move commanded this long with no progress: a wall (or the bridge holding the tracks)
WALL_M = 0.02  # ... progress: moved this far
WALL_DEG = 5.0  # ... or turned this far
WALL_START_S = (
    1.0  # ... never moved at all since the command began: a wall after this (a slow start, review M1)
)
RUN_TURNS_DEG = (0.0, 80.0, -80.0, 40.0, -40.0)  # straight away, sideways (left first), diagonals (§261)
RUN_BEHIND_DEG = 90.0  # a run drives forward only with the person this far off the heading (B, owner §261)
RUN_SPIN_FROM_DEG = 60.0  # the run turns on the spot beyond this, then curves - never a curve's start towards
# the person (the keep-out's ahead is ±60, S4; final review M2)
RUN_TOWARDS_M = 0.05  # a run's route never passes nearer the person than the tank is now, less this (S4)
RUN_MAX_S = 2.8  # the run drives forward at most this long, summed over tries (in the bridge's 3 s, R34.17)
ESCAPE_FROM_M = (
    0.80  # ESCAPE watches the person until they are this near the front (owner, design-notes §257)
)
RUN_SHORTER_M = (0.50, 0.30)  # no run of `cm`: these, when shorter (owner §257)
RUN_GAP_S = 0.25  # a tick this long after the last at most counts as forward time (the bridge's kRunGapMs)
RUN_COUNTS_M = 0.10  # a run try counts once the tank is this far from where it began (LLD §28.25)
RUN_START_S = 1.0  # ... before that, no forward move this long after its start or its last turn: dropped
RUN_TURN_MAX_S = 3.5  # ... or never a forward move this long after it began (a 180 deg spin ~2.8 s)
RUN_TRIES = 3  # run tries per ESCAPE at most (my call, §257)
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
        self.edge = 0  # the newest sighting at the picture's side edge (+1 left, -1 right)
        self.unseen_s = 0.0

    def update(self, inp: SkillInput, dt: float, spinning: bool) -> None:
        p, at = inp.person, inp.at
        if p is not None and p.seen and p.d_m is not None and at is not None and p.t_us != self.t_us:
            self.t_us, self.unseen_s, self.edge = p.t_us, 0.0, p.edge
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
    """FACE "baby: the camera pans to the person within ±35°; beyond, or seen at the picture's side edge (at
    least that far round, LLD §28.12), the tank turns on the spot (from 35 cm only) until within 10° and looks
    again on a newer sighting - at most FACE_LOOKS turns. done facing them, lost when not seen for 1 s. Nobody
    in view: the camera sweeps, then the tank turns in steps when last seen far enough (design-notes §248)."""

    def __init__(self, lim: Limits) -> None:
        super().__init__(lim)
        self._th: float | None = None
        self._looks = 0
        self._turned_at: int | None = None  # the sighting a turn ended on: decide again on a newer one
        self._look_s = 0.0  # when the camera's current look (or the last step's look) began
        self._sweep: list[float] | None = None  # the camera's pans, the side last seen first
        self._sweep_i = 0
        self._side = 1.0  # the steps' direction (+ left)
        self._step_th: float | None = None
        self._steps = 0
        self._t0: float | None = None

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self._clock(now_s, inp)
        if self._t0 is None:
            self._t0 = now_s
        at = inp.at
        if at is None or self.sight.xy is None:
            return self._unseen(now_s, inp)
        if self._th is not None:  # turning: by the odometry, toward where they were
            out = self._spin_to(at, self._th)
            if out is not None:
                return out
            self._th, self._turned_at, self.state = None, self.sight.t_us, "look"
        if self.sight.lost:
            return self._end(Result.LOST, "the person is not in view")
        if self._turned_at is not None and self.sight.t_us == self._turned_at:
            # a sighting from before the turn ended: the camera where they should be, wait for a new one
            return self._halt(self._cam_to(at, FACE_PAN_DEG))
        if not self._turns(at):
            if now_s - self._t0 < FACE_MIN_S - 1e-9:  # facing them already: watch a moment (§251)
                self.state = "watch"
                return self._halt(self._cam_to(at, FACE_PAN_DEG))
            return self._end(Result.DONE, cam=self._cam_to(at, FACE_PAN_DEG))
        self._looks += 1
        self._th, self.state = self.sight.heading(at), "turn"
        return self._spin_to(at, self._th) or self._halt()

    def _unseen(self, now_s: float, inp: SkillInput) -> SkillOut:
        """Nobody in view yet (§248): the camera sweeps - the side last seen first (§237), else ahead first
        (§242) - SWEEP_DWELL_S each; not found, last seen at BLIND_TURN_M or more: turns in TURN_STEP_DEG
        steps toward that side, a look after each, a full circle at most; else lost (no tank turn)."""
        at, p = inp.at, inp.person
        if at is None:
            return self._halt()
        if self._sweep is None:
            self._sweep, self._look_s = self._sweep_pans(p), now_s
            return self._look(self._sweep[0])
        if self._sweep_i < len(self._sweep):
            if now_s - self._look_s < SWEEP_DWELL_S - 1e-9:
                return self._halt()
            self._sweep_i, self._look_s = self._sweep_i + 1, now_s
            if self._sweep_i < len(self._sweep):
                return self._look(self._sweep[self._sweep_i])
            if p is None or p.d_m is None or p.d_m < BLIND_TURN_M - 1e-9:
                return self._end(Result.LOST, "the person is not in view")
            self._side = -1.0 if p.bearing_deg < 0 else 1.0
            return self._look(0.0)
        return self._step(now_s, at)

    def _step(self, now_s: float, at: XYTh) -> SkillOut:
        """The blind steps: TURN_STEP_DEG, then FACE_STEP_LOOK_S still; a full circle, then lost."""
        if self._step_th is not None:
            out = self._spin_to(at, self._step_th)
            if out is not None:
                return out
            self._step_th, self._steps, self._look_s = None, self._steps + 1, now_s
            return self._halt()
        if now_s - self._look_s < FACE_STEP_LOOK_S - 1e-9:
            return self._halt()
        if self._steps * TURN_STEP_DEG >= 360.0 - 1e-9:
            return self._end(Result.LOST, "the person is not in view")
        # FACE_DONE_DEG short of the step is "there": aim past it so each step turns TURN_STEP_DEG
        self._step_th = at[2] + self._side * math.radians(TURN_STEP_DEG + FACE_DONE_DEG)
        return self._spin_to(at, self._step_th) or self._halt()

    def _sweep_pans(self, p: PersonEst | None) -> list[float]:
        """The side last seen first (beyond FACE_LOOK_FROM_DEG), then ahead, then the other side; else ahead
        first, then left, then right (pan + = right)."""
        if p is not None and abs(p.bearing_deg) > FACE_LOOK_FROM_DEG:
            side = max(-FACE_LOOK_PAN_DEG, min(FACE_LOOK_PAN_DEG, -p.bearing_deg))
            return [side, 0.0, -side]
        return [0.0, -FACE_LOOK_PAN_DEG, FACE_LOOK_PAN_DEG]

    def _look(self, pan: float) -> SkillOut:
        self._pan = pan
        return self._halt((pan, self.lim.drive_tilt_deg))

    def _turns(self, at: XYTh) -> bool:
        """Turn the tank: beyond the camera's pan or at the picture's side edge, far enough, looks left."""
        far_round = abs(self.sight.bearing(at)) > FACE_PAN_DEG or self.sight.edge != 0
        return far_round and self.sight.dist(at) >= TURN_FROM_M and self._looks < FACE_LOOKS


class Progress:
    """The wall rule (LLD §28.24): a move commanded at least WALL_S, and in the last WALL_S the pose moved
    less than WALL_M and turned less than WALL_DEG - a wall, or the bridge holding the tracks."""

    def __init__(self) -> None:
        self._since: float | None = None  # when the commanded move began
        self._moved: float | None = None  # when the pose first changed after it (the real tank starts late)
        self._first: XYTh | None = None
        self._trail: list[tuple[float, XYTh]] = []

    def note(self, now_s: float, at: XYTh, moving: bool) -> None:
        if not moving:
            self._since, self._moved, self._first, self._trail = None, None, None, []
            return
        if self._since is None:
            self._since, self._first = now_s, at
        elif self._moved is None and self._first is not None and self._changed(self._first, at):
            self._moved = now_s
        self._trail.append((now_s, at))
        while len(self._trail) > 1 and self._trail[1][0] <= now_s - WALL_S + 1e-9:
            self._trail.pop(0)

    def stuck(self, now_s: float) -> bool:
        if self._since is None or not self._trail:
            return False
        if self._moved is None:  # not moved at all yet: a slow start, or already against a wall
            return now_s - self._since >= WALL_START_S - 1e-9
        if now_s - self._moved < WALL_S - 1e-9:
            return False
        (_, a), (_, b) = self._trail[0], self._trail[-1]
        turned = abs(_wrap(math.degrees(b[2] - a[2])))
        return math.dist(a[:2], b[:2]) < WALL_M and turned < WALL_DEG

    @staticmethod
    def _changed(a: XYTh, b: XYTh) -> bool:
        return math.dist(a[:2], b[:2]) >= 0.005 or abs(_wrap(math.degrees(b[2] - a[2]))) >= 1.0


class EscapeSkill(_Base):
    """ESCAPE "baby cm (owner, design-notes §254; LLD §28.24): the person in view with a fresh picture, else
    no_way. Back off at once, straight back up to BACK_OFF_M over any floor (a wall: no progress); then run
    `run_m` to the first open direction away from them (straight away, then ±30, ±60, ±90 deg, left first),
    one smooth turn, the camera ahead, at most RUN_MAX_S of driving forward; then turn to where they were and
    FACE them: done facing them (no_way when there was no run), lost when not found or the turn is held."""

    def __init__(self, lim: Limits, run_m: float) -> None:
        super().__init__(lim)
        self.run_m = run_m
        self._from: XYTh | None = None
        self._run: RoamSkill | None = None
        self._ran = False  # a run try counted (moved RUN_COUNTS_M)
        self._watch: FaceSkill | None = None
        self._spot: XY | None = None  # the try's spot
        self._tried: set[tuple[float, float]] = set()  # run options tried: (length, turn)
        self._tries = 0
        self._try_from: XYTh | None = None
        self._try_t0 = 0.0  # when this try began
        self._try_s = 0.0  # ... or its last move command (a turn or forward)
        self._try_fwd = False  # this try sent a forward move
        self._fwd_used_s = 0.0  # forward driving, summed over the tries (owner §259, bridge R34.17)
        self._run_prev_s: float | None = None  # the run's previous tick
        self._face: FaceSkill | None = None
        self._th: float | None = None
        self._progress = Progress()

    def tick(self, now_s: float, inp: SkillInput) -> SkillOut:
        self._clock(now_s, inp)
        at = inp.at
        if at is None:
            return self._halt()
        if self.state != "start":
            self._progress.note(now_s, at, self.cmd != STOP)
        if self.state == "start":
            return self._start(now_s, at, inp)
        if self.state == "watch":
            return self._watching(now_s, at, inp)
        if self.state == "back":
            return self._back(now_s, at, inp)
        if self.state == "turn":
            return self._turning(now_s, at, inp)
        if self.state == "run":
            return self._running(now_s, at, inp)
        return self._search(now_s, at, inp)

    def _start(self, now_s: float, at: XYTh, inp: SkillInput) -> SkillOut:
        p, snap = inp.person, inp.snap
        if p is None or not p.seen or p.d_m is None or abs(p.bearing_deg) > VIEW_DEG:
            return self._end(Result.NO_WAY, "the person is not in view")  # it must know where they are
        if snap is None or snap.t_us - p.t_us > FRESH_US:
            return self._end(Result.NO_WAY, "no fresh picture of the person")
        if self._near(inp):
            return self._to_back(at)
        self.state = "watch"
        return self._watching(now_s, at, inp)

    @staticmethod
    def _near(inp: SkillInput) -> bool:
        """A fresh sighting within ESCAPE_FROM_M of the front and in the camera's view ahead (the back-off is
        straight back: a person found aside by the sweep is faced first - S4, final review I1)."""
        p, snap = inp.person, inp.snap
        return (
            p is not None
            and p.seen
            and p.d_m is not None
            and p.d_m <= ESCAPE_FROM_M + 1e-9
            and abs(p.bearing_deg) <= VIEW_DEG
            and snap is not None
            and snap.t_us - p.t_us <= FRESH_US
        )

    def _to_back(self, at: XYTh) -> SkillOut:
        self._from, self.state, self._progress = at, "back", Progress()
        return self._out(Command(-1.0, 0.0), self._cam_to(at, ESCAPE_PAN_DEG))

    def _watching(self, now_s: float, at: XYTh, inp: SkillInput) -> SkillOut:
        """Farther than ESCAPE_FROM_M: FACE them (again each time FACE is done) until they come near."""
        if self._near(inp):
            return self._to_back(at)
        if self._progress.stuck(now_s):  # a turn the bridge holds (R34.14): not for ever
            return self._end(Result.LOST, "could not turn to look")
        if self._watch is None:
            self._watch = FaceSkill(self.lim)
            self._watch._pan = self._pan  # pyright: ignore[reportPrivateUsage]  # the camera is where it is
        out = self._watch.tick(now_s, inp)
        if out.cam is not None:
            self._pan = out.cam[0]
        if out.result == Result.LOST:
            return self._end(Result.LOST, out.detail, out.cam)
        if out.result is not None:  # facing them: watch on with a new FACE
            self._watch = None
            return self._halt(out.cam)
        self.cmd = out.cmd
        return SkillOut(out.cmd, out.cam)

    def _back(self, now_s: float, at: XYTh, inp: SkillInput) -> SkillOut:
        """Straight back (the ramp: at v_min) until BACK_OFF_M along the start heading, or a wall."""
        assert self._from is not None
        x0, y0, th0 = self._from
        moved = -((at[0] - x0) * math.cos(th0) + (at[1] - y0) * math.sin(th0))
        if moved < BACK_OFF_M - 1e-6 and not self._progress.stuck(now_s):
            return self._out(Command(-1.0, 0.0), self._cam_to(at, ESCAPE_PAN_DEG))
        return self._start_run(now_s, at, inp)

    def _start_run(self, now_s: float, at: XYTh, inp: SkillInput) -> SkillOut:
        """The next run try (LLD §28.25): the first open option not tried; none, or RUN_TRIES made: search."""
        self._progress = Progress()
        option = None if self._tries >= RUN_TRIES else self._run_spot(at, inp)
        if option is None:
            return self._to_search(at)
        key, spot = option
        self._tried.add(key)
        self._tries += 1
        self._spot, self._run = spot, None
        self.state, self._pan = "turn", 0.0  # A (owner §261): ESCAPE turns its back itself, the camera ahead
        return self._turning(now_s, at, inp, cam=(0.0, self.lim.drive_tilt_deg))

    def _turning(
        self, now_s: float, at: XYTh, inp: SkillInput, cam: tuple[float, float] | None = None
    ) -> SkillOut:
        """On the spot to the try's spot (within FACE_DONE_DEG); held (the wall rule): the next option."""
        assert self._spot is not None
        if self._progress.stuck(now_s):
            return self._start_run(now_s, at, inp)
        out = self._spin_to(at, math.atan2(self._spot[1] - at[1], self._spot[0] - at[0]))
        if out is not None:
            return SkillOut(out.cmd, cam)
        self._try_from, self._try_t0, self._try_s, self._try_fwd = at, now_s, now_s, False
        self._run = RoamSkill(
            Planner(self.lim, spin_from_deg=RUN_SPIN_FROM_DEG, blind_spin=True),
            "point",
            point=self._spot,
            stand_s=0.0,
            slow_at_point=False,
        )
        self.state = "run"
        return self._running(now_s, at, inp, cam)

    def _behind(self, at: XYTh) -> bool:
        """The person's place RUN_BEHIND_DEG or more off the heading, from the centre (unknown: no)."""
        if self.sight.xy is None:
            return False
        return abs(_wrap(math.degrees(self.sight.heading(at) - at[2]))) >= RUN_BEHIND_DEG - 1e-9

    def _run_spot(self, at: XYTh, inp: SkillInput) -> tuple[tuple[float, float], XY] | None:
        """The first open option not tried: each length (`run_m`, then the shorter RUN_SHORTER_M) in
        RUN_TURNS_DEG's order (owner §259: escaping always goes where it has not looked - no seen-floor
        preference). Open = a route on the cost map never towards the person. Returns ((length, turn),
        spot)."""
        if inp.costs is None or self.sight.xy is None:
            return None
        away = self.sight.heading(at) + math.pi
        cost, routes = inp.costs.base(), RoutePlanner(self.lim)
        for length in [self.run_m, *(m for m in RUN_SHORTER_M if m < self.run_m - 1e-6)]:
            for turn in RUN_TURNS_DEG:
                if (length, turn) in self._tried:
                    continue
                th = away + math.radians(turn)
                spot = (at[0] + length * math.cos(th), at[1] + length * math.sin(th))
                way = routes.route(cost, (at[0], at[1]), spot)
                if way is not None and self._away_all_the_way(at, way):
                    return (length, turn), spot
        return None

    def _away_all_the_way(self, at: XYTh, way: list[XY]) -> bool:
        """Never towards them (S4): no point of the route nearer the person's spot than the tank is now."""
        assert self.sight.xy is not None
        now_m = math.dist(at[:2], self.sight.xy)
        return all(math.dist(w, self.sight.xy) >= now_m - RUN_TOWARDS_M for w in way)

    def _running(
        self, now_s: float, at: XYTh, inp: SkillInput, cam: tuple[float, float] | None = None
    ) -> SkillOut:
        assert self._run is not None and self._try_from is not None
        out = self._run.tick(now_s, inp)
        if out.cmd.v_cms > 0 and not self._behind(at):  # B (owner §261): never forward with them ahead
            return self._to_search(at) if self._ran else self._start_run(now_s, at, inp)
        step = self.lim.dt_s if self._run_prev_s is None else min(now_s - self._run_prev_s, RUN_GAP_S)
        self._run_prev_s = now_s
        if out.cmd.v_cms > 0:
            self._try_fwd = True
            self._fwd_used_s += (
                step  # the time to this tick drove forward (the bridge counts the same, R34.17)
            )
        if out.cmd != STOP:
            self._try_s = now_s  # moving: the 1 s standing counts from its last move
        if not self._ran and math.dist(at[:2], self._try_from[:2]) >= RUN_COUNTS_M - 1e-9:
            self._ran = True
        late = self._fwd_used_s >= RUN_MAX_S - 1e-9
        ended = out.result is not None or self._progress.stuck(now_s)
        if late or (ended and self._ran):
            return self._to_search(at)
        if not self._ran and (ended or self._dropped(now_s)):
            return self._start_run(now_s, at, inp)  # dropped before it counted: the next option
        self.cmd = out.cmd
        return SkillOut(out.cmd, cam)

    def _dropped(self, now_s: float) -> bool:
        """A try that does not get going (LLD §28.25): in its route skill's stuck recovery, standing
        RUN_START_S since its start or its last move, or no forward move RUN_TURN_MAX_S after it began."""
        if getattr(self._run, "state", "") == "recover":
            return True
        if now_s - self._try_s >= RUN_START_S - 1e-9:
            return True
        return not self._try_fwd and now_s - self._try_t0 >= RUN_TURN_MAX_S - 1e-9

    def _to_search(self, at: XYTh) -> SkillOut:
        self.state, self._progress = "search", Progress()
        self._th = None if self.sight.xy is None else self.sight.heading(at)
        return self._halt(self._cam_to(at, FACE_PAN_DEG))

    def _search(self, now_s: float, at: XYTh, inp: SkillInput) -> SkillOut:
        """Turn to the last spot's heading, then FACE; a held turn (no progress) ends lost."""
        if self._progress.stuck(now_s):
            return self._end(Result.LOST, "could not turn to look")
        if self._th is not None:
            out = self._spin_to(at, self._th)
            if out is not None:
                return out
            self._th = None
        if (
            self._face is None
        ):  # final review I1: FACE from nobody in view - only a newer sighting counts, so it
            self._face = FaceSkill(
                self.lim
            )  # looks round (the side the estimate now places them first, §248)
            self._face.sight.t_us = self.sight.t_us
        out = self._face.tick(now_s, inp)
        self.cmd = out.cmd
        if out.result == Result.DONE and not self._ran:
            return self._end(Result.NO_WAY, "no way to run", out.cam)
        if out.result is not None:
            return self._end(out.result, out.detail, out.cam)
        return SkillOut(out.cmd, out.cam)

    def debug(self) -> dict[str, object]:
        return {**super().debug(), "ran": self._ran}


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
        self._planner = Planner(lim, spin_from_deg=FOLLOW_SPIN_FROM_DEG)

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
        if self._planner.inside_turn(at, goal) and abs(self.sight.bearing(at)) <= FOLLOW_PAN_DEG:
            self._route, self.state = (
                None,
                "watch",
            )  # no curve reaches the spot: hold, the camera on them (§250)
            return self._halt(cam)
        if self._route is None or self._goal is None or math.dist(goal, self._goal) > REPLAN_M:
            self._route, self._goal = (
                RoamSkill(self._planner, "point", point=goal, stand_s=0.0, slow_at_point=False),
                goal,
            )
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
