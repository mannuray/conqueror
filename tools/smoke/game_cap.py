"""Measure the game's speed cap on the floor (bridge LLD R34, Plan G1 Task 6; owner: 25 cm/s).

The Uno's envelope caps a track % (PWM share), and the speed is not linear in it (the dead band;
~18 cm/s is the slowest smooth speed, ~33.5 cm/s at 100 %). So it is measured: the tank drives
straight at 50, 55 ... 90 % for 2 s each (drive_rate, heading held), the encoders give the speed -
v = Δcounts / Δt / counts_per_m (the bridge's own, 7514 counts per metre) - and the largest % whose
speed is at most 25 cm/s is the cap. It asks before saving; the save needs the tank's button
(calib, Plan M).

    uv run python game_cap.py              # measure, print the table, ask, save
    uv run python game_cap.py --dry-run    # measure and print only

FLOOR: the tank drives straight ahead about 0.4 m per step, ~9 steps (~3.5 m in all, it eases to a
stop between steps). Clear floor ahead, STOP on the phone within reach.

The floor run of 2026-11-10 (design-notes §233) zig-zagged: a STOP between steps held every other
step's start. Now it eases off with drive_rate 0 between steps, covers 30-60 % (25 cm/s lies below
50 %), measures the two steps either side of the target again, and refuses to pick from a table
whose speed falls with more power or whose repeats disagree by more than AGREE_CMS.
"""

from __future__ import annotations

import argparse
import itertools
import time

from bridge_echo import WsClient

TARGET_CMS = 25.0  # owner, 2026-11-08
STEPS = (30, 35, 40, 45, 50, 55, 60)
AGREE_CMS = 2.0  # two runs of one step further apart than this: something cut a step short
FALL_CMS = 1.0  # more power, slower by more than this: the table is not to be trusted


def speed_cms(counts: int, seconds: float, counts_per_m: int) -> float:
    """cm/s from the mean counts of both tracks over the window."""
    if seconds <= 0 or counts_per_m <= 0:
        raise ValueError("seconds and counts_per_m must be > 0")
    return counts / seconds / counts_per_m * 100.0


def pick_cap(table: list[tuple[int, float]], target: float = TARGET_CMS) -> int | None:
    """The largest % whose measured speed is at most the target, or None: every step too fast,
    or the speed falls with more power (a step cut short - never saved)."""
    rows = sorted(table)
    if any(b[1] < a[1] - FALL_CMS for a, b in itertools.pairwise(rows)):
        return None
    ok = [pct for pct, v in rows if v <= target]
    return max(ok) if ok else None


def mean_of_pair(a: float, b: float) -> float | None:
    """Two runs of one step: their mean, or None when they disagree by more than AGREE_CMS."""
    return None if abs(a - b) > AGREE_CMS else (a + b) / 2


def measure(
    ws: WsClient, pct: int, cpm: int, settle_s: float = 0.6, window_s: float = 1.4
) -> float:
    """Drive straight at pct for settle + window, reading the encoders over the window only."""

    def latest_counts(until: float) -> tuple[int, int] | None:
        last = None
        while time.monotonic() < until:
            ws.send({"t": "drive_rate", "v": pct, "yaw_rate": 0})
            m = ws.recv(0.05)
            if m is not None and m.get("t") == "td":
                last = (int(m["enc"][0]), int(m["enc"][1]))
        return last

    start = latest_counts(time.monotonic() + settle_s)
    t0 = time.monotonic()
    end = latest_counts(t0 + window_s)
    dt = time.monotonic() - t0
    ease = time.monotonic() + 1.0  # ease off to 0 (a STOP would hold the next step's start, §233)
    while time.monotonic() < ease:
        ws.send({"t": "drive_rate", "v": 0, "yaw_rate": 0})
        ws.recv(0.05)
    if start is None or end is None:
        raise RuntimeError("no td telemetry - is the Uno online?")
    counts = ((end[0] - start[0]) + (end[1] - start[1])) // 2
    return speed_cms(counts, dt, cpm)


def measure_all(ws: WsClient, cpm: int, table: list[tuple[int, float]]) -> int | None:
    """Every step once, then the two either side of the target again: the cap, or None."""
    for pct in STEPS:
        v = measure(ws, pct, cpm)
        table.append((pct, v))
        print(f"  {pct:3d} %  {v:5.1f} cm/s")
    cap = pick_cap(table)
    if cap is not None:  # the two steps either side of the target, again: they decide the cap
        again = [p for p in STEPS if p == cap or p == cap + 5]
        for pct in again:
            v2 = measure(ws, pct, cpm)
            v1 = dict(table)[pct]
            mean = mean_of_pair(v1, v2)
            print(
                f"  {pct:3d} %  again {v2:5.1f} cm/s"
                + ("" if mean is not None else "  - DISAGREES")
            )
            if mean is None:
                cap = None
                break
            table = [(p, mean if p == pct else v) for p, v in table]
        ws.send({"t": "drive_rate", "v": 0, "yaw_rate": 0})
        cap = pick_cap(table) if cap is not None else None
    return cap


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default="192.168.31.167")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    ws = WsClient(a.host)
    ws.send({"t": "hello", "client": "game-cap", "proto": 1})
    ws.send({"t": "debug", "on": True, "hz": 10})
    ws.send({"t": "calib"})
    cpm = 7514
    for _ in range(40):
        m = ws.recv(0.1)
        if m is not None and m.get("t") == "calib":
            cpm = int(m["counts_per_m"])
            break
    print(f"counts_per_m {cpm}")
    table: list[tuple[int, float]] = []
    try:
        cap = measure_all(ws, cpm, table)
    finally:
        ws.send({"t": "stop"})  # once, at the very end (between steps it eases off, §233)
    print(
        f"cap for <= {TARGET_CMS} cm/s: {cap} %"
        + ("  (not trusted - nothing to save)" if cap is None else "")
    )
    if cap is None or a.dry_run or input(f"save game_cap_pct {cap}? [y/N] ").strip().lower() != "y":
        ws.close()
        return
    ws.send(
        {"t": "drive", "v": 0, "w": 0}
    )  # take control again (a save needs the driver; 0/0 never moves)
    ws.send({"t": "calib", "game_cap_pct": cap, "id": 1})
    print("press the tank's button to confirm ...")
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        m = ws.recv(0.2)
        if m is not None and m.get("t") == "confirmed":
            print("saved")
            break
    ws.close()


if __name__ == "__main__":
    main()
