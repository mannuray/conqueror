"""A stand-in AI engine for game mode's stand checks (bridge LLD R34, Plan G1 Task 6): no camera, no
person.

It registers as the skill server (hello "ai-skills", with "people" true or false - the seeing-test
lock), streams a scripted person estimate at 10 Hz while the bridge says game on, answers back_ok
questions, and can go silent (the bridge must then hold the tank until STOP).

    uv run python fake_person.py --at 80                     # a person still at 80 cm ahead
    uv run python fake_person.py --approach 80:25:3
    # from 80 cm to 25 cm over 3 s, then stays at 25
    uv run python fake_person.py --approach 80:25:3 --back-to 60:4
    # ... then back to 60 cm 4 s later
    uv run python fake_person.py --at 80 --silent-after 5
    # stops sending after 5 s (socket kept open)
    uv run python fake_person.py --no-people
    # the seeing test "not passed": a game is refused 7061
    uv run python fake_person.py --at 80 --back no           # back_ok answered no

Nothing here drives: the program on /logo does. Tank on the stand, tracks off the ground.
"""

from __future__ import annotations

import argparse
import time
from dataclasses import dataclass
from typing import Any

from bridge_echo import WsClient


@dataclass(frozen=True)
class Script:
    start_cm: float = 80.0
    end_cm: float = 80.0
    over_s: float = 0.0
    back_cm: float | None = None
    back_after_s: float = 0.0
    unseen: bool = False


def distance_at(s: Script, t: float) -> float:
    """The scripted distance (cm) t seconds into the game."""
    if s.back_cm is not None and t >= s.back_after_s:
        return s.back_cm
    if s.over_s <= 0 or t >= s.over_s:
        return s.end_cm
    return s.start_cm + (s.end_cm - s.start_cm) * t / s.over_s


def person_message(s: Script, t: float, last_cm: float | None) -> dict[str, Any]:
    """One `person` message (bridge LLD R34.3); coming from the change since the last one."""
    d = distance_at(s, t)
    coming = 0 if last_cm is None or abs(d - last_cm) < 0.5 else (1 if d < last_cm else -1)
    return {"t": "person", "seen": not s.unseen, "d_cm": round(d), "bearing": 0, "coming": coming,
            "sure": 0.9, "age_ms": 0}  # fmt: skip


def parse_pair(text: str) -> tuple[float, float]:
    a, b = text.split(":")
    return float(a), float(b)


def script_from(a: argparse.Namespace) -> Script:
    if a.approach:
        start, end, over = (float(x) for x in a.approach.split(":"))
    else:
        start = end = a.at
        over = 0.0
    back_cm, back_after = parse_pair(a.back_to) if a.back_to else (None, 0.0)
    return Script(start, end, over, back_cm, back_after, a.unseen)


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--host", default="192.168.31.167")
    ap.add_argument("--at", type=float, default=80.0)
    ap.add_argument("--approach", help="FROM:TO:SECONDS")
    ap.add_argument("--back-to", help="CM:AFTER_SECONDS")
    ap.add_argument("--silent-after", type=float, default=0.0)
    ap.add_argument("--unseen", action="store_true")
    ap.add_argument("--no-people", action="store_true")
    ap.add_argument("--back", choices=["yes", "no"], default="yes")
    a = ap.parse_args()
    s = script_from(a)
    ws = WsClient(a.host)
    ws.send({"t": "hello", "client": "ai-skills", "proto": 1, "people": not a.no_people})
    print(f"registered (people {'false' if a.no_people else 'true'}); waiting for a game run ...")
    game_at: float | None = None
    last_cm: float | None = None
    next_send = next_ping = time.monotonic()
    try:
        while True:
            m = ws.recv(0.02)
            now = time.monotonic()
            if m is not None and m.get("t") == "game":
                game_at = now if m.get("on") else None
                last_cm = None
                print("game", "on" if game_at is not None else "off")
            elif m is not None and m.get("t") == "skill_ask" and m.get("q") == "back_ok":
                ws.send({"t": "skill_answer", "id": m["id"], "value": a.back == "yes"})
                print(f"back_ok {m.get('cm')} cm -> {a.back}")
            elif m is not None and m.get("t") == "ev" and m.get("kind") == "game":
                print("bridge:", m.get("state"), "caught", m.get("caught"))
            if now >= next_ping:
                ws.send(
                    {"t": "ping", "t0": round(now * 1000) % 1_000_000}
                )  # the 3 s silent-client rule
                next_ping = now + 1.0
            t = None if game_at is None else now - game_at
            silent = a.silent_after > 0 and t is not None and t >= a.silent_after
            if t is not None and not silent and now >= next_send:
                msg = person_message(s, t, last_cm)
                last_cm = msg["d_cm"]
                ws.send(msg)
                next_send = now + 0.1
    except KeyboardInterrupt:
        pass
    finally:
        ws.close()


if __name__ == "__main__":
    main()
