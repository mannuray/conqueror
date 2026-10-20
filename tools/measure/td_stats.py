"""Phase 0: gyro drift and ultrasonic no-echo rate from a `td` recording (tools/smoke/td_record.py CSV).

uv run python td_stats.py gyro  rest.csv        # tank standing still, 5 minutes
uv run python td_stats.py sonar sofa.csv        # tank facing a surface, 10 seconds
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from measure_lib import gyro_stats, sonar_stats


def column(rows: list[dict[str, str]], name: str) -> list[float]:
    return [float(r[name]) for r in rows]


def dist(r: dict[str, str]) -> int | None:
    if r.get("dist_fault") in ("1", "True", "true") or r.get("dist_mm") in (None, "", "null", "None"):
        return None
    return int(float(r["dist_mm"]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("what", choices=["gyro", "sonar"])
    ap.add_argument("csv", type=Path)
    a = ap.parse_args()
    with a.csv.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if a.what == "gyro":
        g = gyro_stats(column(rows, "t_s"), column(rows, "yaw"), column(rows, "rate"))
        print(
            f"{g.minutes:.1f} min at rest: heading drift {g.drift_deg_per_min:+.2f} deg/min, "
            f"rate {g.rate_mean_dps:+.3f} +- {g.rate_std_dps:.3f} deg/s"
        )
    else:
        s = sonar_stats(dist(r) for r in rows)
        med = f"{s.median_mm:.0f} mm" if s.median_mm is not None else "-"
        spread = f"{s.spread_mm:.0f} mm" if s.spread_mm is not None else "-"
        print(f"{s.samples} readings: no echo {s.no_echo_pct:.0f} %, median {med}, spread (10-90 %) {spread}")


if __name__ == "__main__":
    main()
