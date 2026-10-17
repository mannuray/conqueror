"""Straight-run report from a td_record.py CSV (spec 02 §F5.3).

Heading profile through the run, estimated sideways drift and the suggested `track_balance`.

    uv run python straight_report.py run.csv
"""

from __future__ import annotations

import csv
import math
import sys
from pathlib import Path

COUNTS_PER_CM = 75.6  # design-notes §63 (owner tape: 160 cm / ~12 100 counts)


def wrap(d: float) -> float:
    return (d + 540.0) % 360.0 - 180.0


def report(path: Path) -> int:
    with path.open() as f:
        rows = list(csv.DictReader(f))
    moving = [r for r in rows if int(r["pwm_l"]) != 0 or int(r["pwm_r"]) != 0]
    if len(moving) < 10:
        print("no driving in this recording")
        return 1
    y0 = float(moving[0]["yaw"])
    n = len(moving)
    marks = [
        (p, wrap(float(moving[min(n - 1, int(n * p) - 1)]["yaw"]) - y0))
        for p in (0.25, 0.5, 0.75, 1.0)
    ]
    print("heading:", "  ".join(f"{int(p * 100)}%: {h:+.2f}°" for p, h in marks))
    dl = int(moving[-1]["enc_l"]) - int(moving[0]["enc_l"])
    dr = int(moving[-1]["enc_r"]) - int(moving[0]["enc_r"])
    lateral = 0.0
    prev = int(moving[0]["enc_l"]) + int(moving[0]["enc_r"])
    for r in moving[1:]:
        cur = int(r["enc_l"]) + int(r["enc_r"])
        step_cm = (cur - prev) / 2 / COUNTS_PER_CM
        lateral += step_cm * math.sin(math.radians(wrap(float(r["yaw"]) - y0)))
        prev = cur
    cm = (dl + dr) / 2 / COUNTS_PER_CM
    print(f"distance ≈ {cm:.0f} cm, sideways ≈ {lateral:+.1f} cm (+ = left), counts L {dl} R {dr}")
    if dl > 0:
        print(
            f"suggested track_balance ≈ {round((dr / dl - 1) * 1000)} ‰ (this run's count ratio - "
            "meaningful from a normal run with the gyro hold on; with the hold off the counts are forced equal)"
        )
    return 0


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: straight_report.py run.csv")
        return 2
    return report(Path(sys.argv[1]))


if __name__ == "__main__":
    raise SystemExit(main())
