"""Where the tank is (AI engine LLD §14, design-notes §132). Pure: telemetry samples in, poses out.

- Heading: the tank's own, as `td` sends it - the Uno re-learns the gyro bias at rest and the bridge applies
  the gyro scale, so it is used as it comes; unwrapped across 360, the room's 0 at the first sample. Counter-
  clockwise positive, like the room's theta.
- Distance: the mean of the two tracks' encoder counts / counts_per_m. The encoders have one channel, so each
  track's direction is the sign of its PWM output - the output of the sample before, because a sample's count
  change happened during the 20 ms before it - and the last non-zero sign while it coasts; a stalled track
  adds nothing.
- A restart (the Uno's clock or the bridge's going back) re-anchors: no step, no jump. A gap over 0.5 s adds
  no distance (unknown) but keeps the turn made in it: the heading is absolute within one Uno run (review I2).
- Pose.th_rad is always within -pi..pi; the turning so far is in the drift."""

from __future__ import annotations

import math
from collections import deque

from conqueror_ai.domain.types import Pose, TelemSample

GAP_S = 0.5  # longer without a sample: no step across it
GAP_SPEED_MPS = 0.3  # the drift a gap may hide: the tank's fastest straight speed x the gap
DRIFT_DIST = 0.03  # of the distance driven (Spec 7 §7)
DRIFT_DEG_PER_TURN = 1.0  # per 360 deg turned
SPEED_WINDOW_US = 200_000
HISTORY_US = 2_000_000
STALL_L, STALL_R = 1 << 3, 1 << 4  # FastFlagBit STALL_L / STALL_R


def _wrap180(d: float) -> float:
    return (d + 180.0) % 360.0 - 180.0


class PoseFilter:
    def __init__(self) -> None:
        self._cpm: int | None = None
        self._last: TelemSample | None = None
        self._sign = [0, 0]  # the last non-zero PWM sign per track
        self._x = self._y = self._th_deg = 0.0
        self._steps: deque[tuple[int, float]] = deque()  # (t_us, d) for the speed
        self._drift_m = 0.0
        self._drift_deg = 0.0
        self._pose: Pose | None = None

    def set_counts_per_m(self, n: int) -> None:
        if n > 0:  # a broken reply must not divide by 0
            self._cpm = n

    def pose(self) -> Pose | None:
        return self._pose

    def drift(self) -> tuple[float, float]:
        """How far off the pose could be: (m, deg)."""
        return self._drift_m, self._drift_deg

    def update(self, s: TelemSample) -> Pose:
        last, self._last = self._last, s
        d = 0.0
        if last is not None:
            gap_s = (s.bus_us - last.bus_us) / 1e6
            if self._restarted(last, s):
                self._steps.clear()  # the old stamps are from another clock (Plan U review I1)
            elif gap_s > GAP_S:
                self._gap(last, s, gap_s)
                self._steps.clear()
            else:
                d = self._step(last, s)
        for i, pwm in enumerate(
            (s.pwm_l, s.pwm_r)
        ):  # after the step: its counts ran under the earlier output
            if pwm != 0:
                self._sign[i] = 1 if pwm > 0 else -1
        self._steps.append((s.bus_us, d))
        while self._steps and s.bus_us - self._steps[0][0] > SPEED_WINDOW_US:
            self._steps.popleft()
        span_us = s.bus_us - self._steps[0][0]
        moved = sum(x for _, x in list(self._steps)[1:])  # the oldest one's step lies before the window
        v = moved / (span_us / 1e6) if span_us > 0 else 0.0
        th = math.radians(_wrap180(self._th_deg))
        self._pose = Pose(s.bus_us, self._x, self._y, th, v, math.radians(s.rate_dps))
        return self._pose

    @staticmethod
    def _restarted(last: TelemSample, s: TelemSample) -> bool:
        return s.uno_ms < last.uno_ms or s.bus_us < last.bus_us

    def _gap(self, last: TelemSample, s: TelemSample, gap_s: float) -> None:
        """No distance across a gap (unknown), but the turn made in it is kept. If the tank could have turned
        more than half a turn in it, the wrap may hide a whole turn: the heading is then unsure by 360."""
        dth = _wrap180(s.yaw_deg - last.yaw_deg)
        self._th_deg += dth
        self._drift_deg += abs(dth) / 360.0 * DRIFT_DEG_PER_TURN
        if max(abs(last.rate_dps), abs(s.rate_dps)) * gap_s > 180.0:
            self._drift_deg += 360.0
        self._drift_m += GAP_SPEED_MPS * gap_s

    def _step(self, last: TelemSample, s: TelemSample) -> float:
        dth = _wrap180(s.yaw_deg - last.yaw_deg)
        th_mid = math.radians(self._th_deg + dth / 2.0)
        self._th_deg += dth
        self._drift_deg += abs(dth) / 360.0 * DRIFT_DEG_PER_TURN
        if self._cpm is None:
            return 0.0
        dl = 0 if s.flags & STALL_L else max(0, s.enc_l - last.enc_l) * self._sign[0]
        dr = 0 if s.flags & STALL_R else max(0, s.enc_r - last.enc_r) * self._sign[1]
        d = (dl + dr) / 2.0 / self._cpm
        self._x += d * math.cos(th_mid)
        self._y += d * math.sin(th_mid)
        self._drift_m += DRIFT_DIST * abs(d)
        return d


class PoseHistory:
    """The last 2 s of poses; the pose at any time inside them, interpolated (theta on the circle)."""

    def __init__(self) -> None:
        self._poses: deque[Pose] = deque()

    def push(self, p: Pose) -> None:
        if self._poses and p.t_us < self._poses[-1].t_us:
            self._poses.clear()  # the bridge restarted: its clock went back (Plan U review I1)
        self._poses.append(p)
        while p.t_us - self._poses[0].t_us > HISTORY_US:
            self._poses.popleft()

    def at(self, t_us: int) -> Pose | None:
        ps = self._poses
        if not ps or t_us < ps[0].t_us or t_us > ps[-1].t_us:
            return None
        for a, b in zip(ps, list(ps)[1:], strict=False):
            if a.t_us <= t_us <= b.t_us:
                f = 0.0 if b.t_us == a.t_us else (t_us - a.t_us) / (b.t_us - a.t_us)
                dth = (b.th_rad - a.th_rad + math.pi) % (2 * math.pi) - math.pi
                th = (a.th_rad + f * dth + math.pi) % (2 * math.pi) - math.pi
                return Pose(
                    t_us,
                    a.x_m + f * (b.x_m - a.x_m),
                    a.y_m + f * (b.y_m - a.y_m),
                    th,
                    a.v_mps + f * (b.v_mps - a.v_mps),
                    a.w_rps + f * (b.w_rps - a.w_rps),
                )
        return ps[-1]
