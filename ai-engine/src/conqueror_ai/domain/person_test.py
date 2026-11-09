"""Spec 10 phase 0 (Plan P0): can the tank see a crawling person? A person box turned into a distance from
the tank's front edge and a direction, and each taped mark scored against the owner's bar (2026-11-08): seen
in >= 8 of 10 pictures at 0.5-2 m, the distance within 10 cm inside 1 m and 20 % beyond, the direction within
5 degrees, and inside 50 cm straight ahead the sonar reading them within 10 cm. Measurement only: nothing here
feeds the tracker, the memory or the map."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

from conqueror_ai.domain.person import CUT_PX, PersonRange, person_range, pick_person

__all__ = ["CUT_PX", "PersonRange", "person_range", "pick_person"]  # P0's names, now shared (G2)

SHARE = 0.8  # 8 of 10
NEAR_TOL_M = 0.10  # the distance's tolerance up to 1 m
FAR_TOL = 0.20  # beyond 1 m, a share of the tape
DIR_TOL_DEG = 5.0
SONAR_TOL_M = 0.10
CAMERA_FROM_M = 0.5  # nearer marks are the sonar's case: reported, not scored for the camera
SONAR_UP_TO_M = 0.5


@dataclass(frozen=True)
class Mark:
    """One taped mark: the owner's hands this far from the front edge, along this direction (deg, + left)."""

    mid: str
    tape_m: float
    angle_deg: float
    still: bool
    tilt_deg: float


@dataclass(frozen=True)
class MarkScore:
    mark: Mark
    pictures: int
    seen: int
    ranged_ok: int
    dir_ok: int
    sonar_ok: int
    sonar_n: int
    camera_scored: bool
    passed: bool
    why: str


def _dist_ok(tape_m: float, d: float) -> bool:
    tol = NEAR_TOL_M if tape_m <= 1.0 else FAR_TOL * tape_m
    return abs(d - tape_m) <= tol + 1e-9


def score_mark(
    mark: Mark, per_picture: Sequence[PersonRange | None], sonar_mm: Sequence[int | None]
) -> MarkScore:
    """One mark against the bar. per_picture: the picked person per scored picture (None: no person seen).
    A crawling mark is graded on seen and distance only: crawling across the mark moves the person
    sideways, so its direction and the sonar are reported, not graded (P0 review I2)."""
    n = len(per_picture)
    got = [r for r in per_picture if r is not None]
    ranged_ok = sum(1 for r in got if r.dist_m is not None and _dist_ok(mark.tape_m, r.dist_m))
    dir_ok = sum(
        1
        for r in got
        if r.bearing_deg is not None and abs(r.bearing_deg - mark.angle_deg) <= DIR_TOL_DEG + 1e-9
    )
    camera_scored = mark.tape_m >= CAMERA_FROM_M - 1e-9
    fails: list[str] = []
    if camera_scored:
        if len(got) < SHARE * n or n == 0:
            fails.append(f"seen {len(got)}/{n}")
        elif ranged_ok < SHARE * len(got):
            fails.append(f"distance {ranged_ok}/{len(got)}")
        if mark.still and got and dir_ok < SHARE * len(got):  # crawling across moves them sideways
            fails.append(f"direction {dir_ok}/{len(got)}")
    sonar_ok = sum(
        1 for mm in sonar_mm if mm is not None and abs(mm / 1000.0 - mark.tape_m) <= SONAR_TOL_M + 1e-9
    )
    near_ahead = mark.still and mark.tape_m <= SONAR_UP_TO_M + 1e-9 and mark.angle_deg == 0.0
    if near_ahead and (not sonar_mm or sonar_ok < SHARE * len(sonar_mm)):
        fails.append(f"sonar {sonar_ok}/{len(sonar_mm)}")
    return MarkScore(
        mark,
        n,
        len(got),
        ranged_ok,
        dir_ok,
        sonar_ok,
        len(sonar_mm),
        camera_scored,
        not fails,
        ", ".join(fails),
    )


def verdict(detector: str, scores: Sequence[MarkScore]) -> str:
    """Every mark's line, then the detector's overall answer: PASS only when every mark passes."""
    lines = [
        f"{s.mark.mid}: {'pass' if s.passed else 'FAIL'}  seen {s.seen}/{s.pictures}"
        f"  distance {s.ranged_ok}/{s.seen}  direction {s.dir_ok}/{s.seen}  sonar {s.sonar_ok}/{s.sonar_n}"
        + ("" if s.camera_scored else "  (camera not scored)")
        + (f"  - {s.why}" if s.why else "")
        for s in scores
    ]
    ok = sum(1 for s in scores if s.passed)
    lines.append(f"{detector}: {'PASS' if ok == len(scores) else 'FAIL'} ({ok} of {len(scores)} marks pass)")
    return "\n".join(lines)
