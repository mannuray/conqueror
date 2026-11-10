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


# ---- the simple seeing test (design-notes §234): one crawl straight ahead, the sonar as the ruler
@dataclass(frozen=True)
class WalkBand:
    name: str
    lo: float  # the sonar's distance (m), from
    hi: float  # ... to (not included)
    centre: float


BANDS = (
    WalkBand("0.3", 0.25, 0.40, 0.3),
    WalkBand("0.5", 0.40, 0.60, 0.5),
    WalkBand("1.0", 0.85, 1.15, 1.0),
    WalkBand("1.5", 1.30, 1.70, 1.5),
    WalkBand("2.0", 1.80, 2.20, 2.0),
)
BAND_PICTURES = 5  # a band with fewer pictures was not crawled through


@dataclass(frozen=True)
class BandScore:
    band: WalkBand
    pictures: int
    seen: int
    ranged_ok: int
    camera_scored: bool
    passed: bool
    why: str


def _walk_dist_ok(sonar_m: float, r: PersonRange) -> bool:
    return (
        r.dist_m is not None
        and abs(r.dist_m - sonar_m) <= (NEAR_TOL_M if sonar_m <= 1.0 else FAR_TOL * sonar_m) + 1e-9
    )


def score_walk(samples: Sequence[tuple[float | None, PersonRange | None]]) -> list[BandScore]:
    """Each picture's sonar reading (m, None: no echo) and the camera's person; P0's bar in each band: enough
    pictures, seen in 8 of 10 (a box cut by the picture's bottom is seen, close), and from 0.5 m the camera's
    distance within the tolerance of the sonar's in 8 of 10 seen."""
    out: list[BandScore] = []
    for b in BANDS:
        got = [(s, r) for s, r in samples if s is not None and b.lo <= s < b.hi]
        seen = [(s, r) for s, r in got if r is not None]
        ranged = sum(1 for s, r in seen if _walk_dist_ok(s, r))
        scored = b.centre >= CAMERA_FROM_M - 1e-9
        fails: list[str] = []
        if len(got) < BAND_PICTURES:
            fails.append(f"only {len(got)} pictures (crawl through it slowly)")
        elif len(seen) < SHARE * len(got) - 1e-9:
            fails.append("not seen often enough")
        elif scored and ranged < SHARE * len(seen) - 1e-9:
            fails.append("distance too far off the sonar's")
        out.append(BandScore(b, len(got), len(seen), ranged, scored, not fails, ", ".join(fails)))
    return out


def walk_verdict(detector: str, scores: Sequence[BandScore]) -> str:
    """Every band's line, then the answer: PASS only when every band passes."""
    lines = [
        f"{s.band.name}: {'pass' if s.passed else 'FAIL'}  pictures {s.pictures}  seen {s.seen}"
        + (f"  distance {s.ranged_ok}/{s.seen}" if s.camera_scored else "  (distance: the sonar's case)")
        + (f"  - {s.why}" if s.why else "")
        for s in scores
    ]
    ok = sum(1 for s in scores if s.passed)
    lines.append(f"{detector}: {'PASS' if ok == len(scores) else 'FAIL'} ({ok} of {len(scores)} bands pass)")
    return "\n".join(lines)
