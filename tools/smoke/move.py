"""MOVE by distance over WiFi (spec 02 Part G, bridge LLD R7.7): moves and counts_per_m calibration.

    uv run python move.py 100 --speed 40      # forward 100 cm
    uv run python move.py -50                 # reverse 50 cm
    uv run python move.py --calibrate         # move 100 cm, type the tape distance, average, save
    uv run python move.py --gyro-scale 10230  # save the gyro scale (LLD R19, the tank's button)

Takes control (the move command does), so release the phone page first.
STOP from the phone or the IR remote ends a move.
"""

from __future__ import annotations

import argparse
import contextlib
import threading
import time
from collections.abc import Callable
from typing import Any, Protocol

from bridge_echo import WsClient

Message = dict[str, Any]


class WsLike(Protocol):
    def send(self, obj: dict[str, Any]) -> None: ...
    def recv(self, timeout: float) -> dict[str, Any] | None: ...
    def close(self) -> None: ...


def counts_travelled(start: tuple[int, int], end: tuple[int, int]) -> int:
    """Mean of both tracks' counts - the Uno counts rising edges, so they rise in reverse too."""
    return ((end[0] - start[0]) + (end[1] - start[1])) // 2


def new_cpm(counts: int, measured_cm: float) -> int:
    """Counts per metre from the counts travelled (roll-on included) and the tape distance."""
    if measured_cm <= 0:
        raise ValueError("measured distance must be > 0 cm")
    return round(counts * 100 / measured_cm)


def mean_cpm(runs: list[int]) -> int:
    if not runs:
        raise ValueError("no calibration runs")
    return round(sum(runs) / len(runs))


def wrap_deg(d: float) -> float:
    return (d + 540.0) % 360.0 - 180.0


class Session:
    """One WebSocket client: debug stream (10 Hz) for the counts, pings keep it connected."""

    def __init__(self, host: str = "", ws: WsLike | None = None) -> None:
        self.ws: WsLike = ws if ws is not None else WsClient(host)
        self.ws.send({"t": "hello", "client": "move-tool", "proto": 1})
        self.ws.send({"t": "debug", "on": True, "hz": 10})
        self.last_td: Message | None = None
        self.last_ping = time.monotonic()
        self.next_id = 100

    def pump_until(self, want: Callable[[Message], bool], seconds: float) -> Message | None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            now = time.monotonic()
            if now - self.last_ping >= 1.0:  # the bridge closes a client silent for 3 s
                self.ws.send({"t": "ping", "t0": int(now * 1000)})
                self.last_ping = now
            msg = self.ws.recv(0.2)
            if msg is None:
                continue
            if msg.get("t") == "td":
                self.last_td = msg
            if want(msg):
                return msg
        return None

    def ask(self, prompt: str, reader: Callable[[str], str] = input) -> str:
        """input() with pings meanwhile: the bridge drops a client that is silent for 3 s."""
        answer: list[str] = []
        error: list[BaseException] = []

        def read() -> None:
            try:
                answer.append(reader(prompt))
            except BaseException as e:  # EOF / closed stdin must end the tool, never drive again
                error.append(e)

        t = threading.Thread(target=read, daemon=True)
        t.start()
        while t.is_alive():
            self.pump_until(lambda _m: False, 0.3)
        if error:
            raise error[0]
        return answer[0] if answer else ""

    def fresh_td(self) -> Message:
        self.last_td = None
        msg = self.pump_until(lambda m: m.get("t") == "td", 2.0)
        if msg is None:
            raise RuntimeError("no debug telemetry (td) from the bridge - is the Uno online?")
        return msg

    def counts_per_m(self) -> int:
        self.ws.send({"t": "calib"})
        msg = self.pump_until(lambda m: m.get("t") == "calib", 3.0)
        if msg is None:
            raise RuntimeError("no calib reply - bridge without Plan K firmware?")
        return int(msg["counts_per_m"])

    def save_counts_per_m(self, cpm: int) -> bool:
        return self.save_calib("counts_per_m", cpm)

    def save_gyro_scale(self, scale_e4: int) -> bool:
        """Bridge LLD R19: the gyro scale in parts per 10 000 (10230 = x1.023)."""
        return self.save_calib("gyro_scale", scale_e4)

    def save_calib(self, key: str, value: int) -> bool:
        self.next_id += 1
        mid = self.next_id
        # Control is handed back 5 s after a move: take it again (drive 0/0 never moves the tank).
        self.ws.send({"t": "drive", "v": 0, "w": 0})
        self.ws.send({"t": "calib", key: value, "id": mid})
        # Plan O (design-notes §91): a save waits for a press of the tank's button (30 s).
        msg = self.pump_until(
            lambda m: (
                (m.get("t") == "nack" and m.get("id") == mid)
                or (m.get("t") == "confirm" and m.get("what") == "calib")
            ),
            3.0,
        )
        if msg is None or msg.get("t") == "nack":
            print(f"save refused: {msg}")
            return False
        print("press the tank's button to save (30 s) ...")
        msg = self.pump_until(
            lambda m: (
                m.get("t") in ("confirmed", "confirm_expired")
                or (m.get("t") == "nack" and m.get("id") == mid)
            ),
            35.0,
        )
        if msg is None or msg.get("t") != "confirmed":
            print(f"not saved: {msg.get('t') if msg else 'no answer'}")
            return False
        return True

    def move(self, cm: int, speed: int) -> tuple[Message, int, float]:
        """Runs one move: (primitive_done, counts travelled after the roll-on, heading change)."""
        start = self.fresh_td()
        self.next_id += 1
        mid = self.next_id
        self.ws.send({"t": "move", "dist_cm": cm, "speed": speed, "id": mid})
        reply = self.pump_until(lambda m: m.get("id") == mid and m.get("t") in ("ack", "nack"), 3.0)
        if reply is None or reply.get("t") == "nack":
            raise RuntimeError(f"move refused: {reply}")
        done = self.pump_until(
            lambda m: m.get("kind") == "primitive_done" and m.get("prim") == "move",
            abs(cm) / 5 + 10,
        )
        if done is None:
            raise RuntimeError("no primitive_done - press STOP")
        self.pump_until(lambda _m: False, 0.5)  # roll-on
        end = self.fresh_td()
        counts = counts_travelled(
            (int(start["enc"][0]), int(start["enc"][1])),
            (int(end["enc"][0]), int(end["enc"][1])),
        )
        return done, counts, wrap_deg(float(end["yaw"]) - float(start["yaw"]))

    def close(self) -> None:
        with contextlib.suppress(OSError):
            self.ws.send({"t": "debug", "on": False})
            self.ws.send({"t": "release_control"})
        self.ws.close()


def report(done: Message, counts: int, heading: float, cpm: int) -> None:
    after = counts * 100 / cpm
    print(
        f"{done.get('reason')}: counted {done.get('moved_cm')} cm at the stop, "
        f"{after:.1f} cm after the "
        f"roll-on, heading {heading:+.1f} deg (counts {counts}, counts_per_m {cpm})"
    )


def calibrate(s: Session, speed: int) -> int:
    cpm = s.counts_per_m()
    runs: list[int] = []
    while True:
        s.ask("Place the tank at the start of the 1 m tape, then press Enter to drive 100 cm ...")
        done, counts, heading = s.move(100, speed)
        report(done, counts, heading, cpm)
        text = s.ask("How far did it go on the tape (cm)? (blank = discard this run) ").strip()
        if text:
            runs.append(new_cpm(counts, float(text)))
            print(f"run {len(runs)}: {runs[-1]} counts/m - mean of {len(runs)}: {mean_cpm(runs)}")
        choice = s.ask("[a]gain, [s]ave the mean, [q]uit without saving? ").strip().lower()
        if choice == "s" and runs:
            mean = mean_cpm(runs)
            if s.save_counts_per_m(mean):
                print(f"saved counts_per_m {mean} (was {cpm})")
                return 0
            return 1
        if choice != "a":  # only an explicit "a" drives again
            return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="MOVE by distance / counts_per_m calibration (Plan K)")
    ap.add_argument("cm", type=int, nargs="?", help="distance, + forward, - reverse (1..500)")
    ap.add_argument("--speed", type=int, default=40)
    ap.add_argument("--calibrate", action="store_true")
    ap.add_argument(
        "--gyro-scale", type=int, default=None, help="save the gyro scale, 9000..11000 (R19)"
    )
    ap.add_argument("--host", default="conqueror-bridge.local")
    args = ap.parse_args()
    if not args.calibrate and args.cm is None and args.gyro_scale is None:
        ap.error("give a distance in cm, --calibrate or --gyro-scale")
    s = Session(args.host)
    try:
        if args.gyro_scale is not None:
            return 0 if s.save_gyro_scale(args.gyro_scale) else 1
        if args.calibrate:
            return calibrate(s, args.speed)
        cm = args.cm
        if cm is None:  # checked above; kept for the type checker
            return 2
        done, counts, heading = s.move(int(cm), args.speed)
        report(done, counts, heading, s.counts_per_m())
        return 0 if done.get("ok") else 1
    finally:
        s.close()


if __name__ == "__main__":
    raise SystemExit(main())
