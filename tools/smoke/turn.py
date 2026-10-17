"""Arcs and pivots over WiFi (spec 02 Part H, bridge LLD R8.5).

    uv run python turn.py 90 --radius 60 --speed 40     # arc left 90° on a 60 cm radius
    uv run python turn.py -90 --pivot                    # pivot right 90°

Prints the end heading change and the end position (x forward, y left, cm) from the encoders
and the gyro, next to the geometric point. STOP from the phone or the IR remote ends a turn.
"""

from __future__ import annotations

import argparse
import itertools
import math
from typing import Any

from move import Session, wrap_deg

TRACK_HALF_CM = 9.5  # B / 2: a pivot turns around the inside track


def integrate(samples: list[tuple[int, float]], counts_per_cm: float) -> tuple[float, float]:
    """Path from (mean counts, yaw °) samples: x forward, y left of the start pose, in cm."""
    x = y = 0.0
    yaw0 = samples[0][1]
    for (c0, a0), (c1, a1) in itertools.pairwise(samples):
        ds = (c1 - c0) / counts_per_cm
        h = (
            math.radians(wrap_deg((a0 + a1) / 2 - yaw0))
            if abs(a1 - a0) < 180
            else math.radians(wrap_deg(a1 - yaw0))
        )
        x += ds * math.cos(h)
        y += ds * math.sin(h)
    return x, y


def arc_point(radius_cm: float, deg: float) -> tuple[float, float]:
    """Where an ideal arc of `deg` (+ left) on `radius_cm` ends, from the start pose."""
    t = math.radians(abs(deg))
    side = 1.0 if deg >= 0 else -1.0
    return radius_cm * math.sin(t), side * radius_cm * (1.0 - math.cos(t))


def run(s: Session, deg: float, radius: int, pivot: bool, speed: int) -> int:
    cpm = s.counts_per_m()
    samples: list[tuple[int, float]] = []

    def keep(m: dict[str, Any]) -> None:
        if m.get("t") == "td":
            samples.append(((int(m["enc"][0]) + int(m["enc"][1])) // 2, float(m["yaw"])))

    start = s.fresh_td()
    keep(start)
    s.next_id += 1
    mid = s.next_id
    cmd: dict[str, Any] = {"t": "turn", "by": deg, "speed": speed, "id": mid}
    if radius:
        cmd["radius_cm"] = radius
    if pivot:
        cmd["pivot"] = True
    s.ws.send(cmd)

    def answered(m: dict[str, Any]) -> bool:
        keep(m)
        return m.get("id") == mid and m.get("t") in ("ack", "nack")

    reply = s.pump_until(answered, 3.0)
    if reply is None or reply.get("t") == "nack":
        print(f"refused: {reply.get('message') if reply else 'no answer'}")
        return 1

    def finished(m: dict[str, Any]) -> bool:
        keep(m)
        return m.get("kind") == "primitive_done" and m.get("id") == mid

    done = s.pump_until(finished, 60.0)
    s.pump_until(lambda m: (keep(m), False)[1], 0.5)  # roll-on
    if done is None:
        print("no primitive_done - press STOP")
        return 1
    x, y = integrate(samples, cpm / 100.0)
    turned = wrap_deg(samples[-1][1] - samples[0][1])
    want = arc_point(radius if radius else TRACK_HALF_CM, deg) if (radius or pivot) else (0.0, 0.0)
    print(
        f"{done.get('reason')}: heading {turned:+.1f} deg (asked {deg:+.0f}), "
        f"end x {x:.1f} y {y:+.1f} cm "
        f"(ideal x {want[0]:.1f} y {want[1]:+.1f})"
    )
    return 0 if done.get("ok") else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Arc / pivot / spin turn (Plan L)")
    ap.add_argument("deg", type=float, help="+ left, - right")
    ap.add_argument("--radius", type=int, default=0)
    ap.add_argument("--pivot", action="store_true")
    ap.add_argument("--speed", type=int, default=40)
    ap.add_argument("--host", default="conqueror-bridge.local")
    args = ap.parse_args()
    s = Session(args.host)
    s.ws.send({"t": "debug", "on": True, "hz": 50})
    try:
        return run(s, args.deg, args.radius, args.pivot, args.speed)
    finally:
        s.close()


if __name__ == "__main__":
    raise SystemExit(main())
