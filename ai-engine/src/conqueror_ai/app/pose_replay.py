"""`cq-pose`: replay a recorded session through the pose filter and say where the tank ended - the floor check
of AI engine LLD §14.5 (a Logo square ends at its start; a 2 m out-and-back turns at 2.00 m).

    uv run cq-pose recordings/20261023-181500
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path

from conqueror_ai.adapters.session import replay
from conqueror_ai.domain.pose import PoseFilter
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Pose


class NoCalibration(Exception):
    """The recording has no `calib` reply, so the distance per encoder count is unknown."""


@dataclass(frozen=True)
class PoseSummary:
    samples: int
    counts_per_m: int
    start: Pose
    end: Pose
    farthest_m: float  # the farthest the tank got from its start
    driven_m: float  # the path's length
    turned_deg: float  # all turning, either way
    drift_m: float
    drift_deg: float


def _counts_per_m(folder: Path) -> int | None:
    """The recording's first `calib` reply. It is one number for the whole session, so it counts from the
    start even if the reply came after the first samples (Plan U review M1)."""
    for e in replay(folder):
        if e.text is not None:
            m = json.loads(e.text)
            if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
                return int(m["counts_per_m"])
    return None


def summarize(folder: Path) -> PoseSummary:
    f = PoseFilter()
    cpm = _counts_per_m(folder)
    if cpm is None:
        raise NoCalibration(
            f"no calib reply in {folder} - record with cq-record, which asks the bridge for it"
        )
    f.set_counts_per_m(cpm)
    start: Pose | None = None
    prev: Pose | None = None
    samples = 0
    farthest = driven = turned = 0.0
    for e in replay(folder):
        if e.text is None:
            continue
        s = parse_td(json.loads(e.text))
        if s is None:
            continue
        p = f.update(s)
        samples += 1
        if start is None:
            start = p
        if prev is not None:
            driven += math.hypot(p.x_m - prev.x_m, p.y_m - prev.y_m)
            turned += abs((p.th_rad - prev.th_rad + math.pi) % (2 * math.pi) - math.pi)
        farthest = max(farthest, math.hypot(p.x_m - start.x_m, p.y_m - start.y_m))
        prev = p
    if start is None or prev is None:
        raise ValueError(f"no telemetry in {folder}")
    dm, dd = f.drift()
    return PoseSummary(samples, cpm, start, prev, farthest, driven, math.degrees(turned), dm, dd)


def report(s: PoseSummary) -> str:
    e = s.end
    return "\n".join(
        [
            f"{s.samples} samples, counts_per_m {s.counts_per_m}",
            f"end: x {e.x_m * 100:+.1f} cm, y {e.y_m * 100:+.1f} cm (left +), "
            f"heading {math.degrees(e.th_rad):+.1f} deg",
            f"end from the start: {math.hypot(e.x_m, e.y_m) * 100:.1f} cm",
            f"farthest from the start: {s.farthest_m * 100:.1f} cm",
            f"driven {s.driven_m * 100:.0f} cm, turned {s.turned_deg:.0f} deg",
            f"could be off by: {s.drift_m * 100:.1f} cm, {s.drift_deg:.1f} deg",
        ]
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path)
    a = ap.parse_args()
    try:
        print(report(summarize(a.folder)))
    except (NoCalibration, ValueError) as err:
        raise SystemExit(str(err)) from None


if __name__ == "__main__":
    main()
