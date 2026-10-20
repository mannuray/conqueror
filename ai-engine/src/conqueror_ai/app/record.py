"""`cq-record`: watch the tank - camera and telemetry - record the session and show it live (Spec 7 phase 1).

Read only: it never moves the tank (drive the tank with the phone meanwhile). Ctrl-C ends it and prints a
summary: pictures and telemetry per second, and the pictures' age on arrival.

    uv run cq-record --host 192.168.31.167 [--seconds 60] [--no-view] [--out recordings/<now>]
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import statistics
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.domain.types import Frame, TelemSample

DEFAULT_HOST = "192.168.31.167"  # the tank on the home network (mDNS does not resolve from Python here)


class View(Protocol):
    """What the recorder shows: RerunView in real use, NoView in tests and with --no-view."""

    def telemetry(self, s: TelemSample) -> None: ...
    def frame(self, f: Frame, age_s: float | None) -> None: ...


class NoView:
    def telemetry(self, s: TelemSample) -> None:
        pass

    def frame(self, f: Frame, age_s: float | None) -> None:
        pass


@dataclass
class Summary:
    seconds: float = 0.0
    pictures: int = 0
    samples: int = 0
    ages_ms: list[float] = field(default_factory=list[float])

    def text(self) -> str:
        if self.seconds <= 0:
            return "nothing recorded"
        age = (
            f"age on arrival median {statistics.median(self.ages_ms):.0f} ms "
            f"(min {min(self.ages_ms):.0f}, max {max(self.ages_ms):.0f})"
            if self.ages_ms
            else "age on arrival unknown (no pong with bridge_us)"
        )
        return (
            f"{self.seconds:.1f} s: {self.pictures / self.seconds:.1f} pictures/s, "
            f"{self.samples / self.seconds:.1f} telemetry/s; {age}"
        )


async def record(
    link: BridgeLink, cam: MjpegSource, rec: SessionRecorder, view: View, seconds: float | None
) -> Summary:
    stop = asyncio.Event()
    link.tap = rec.telemetry
    cam.raw_tap = []
    tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]
    out = Summary()
    t0 = time.monotonic()
    try:
        while seconds is None or time.monotonic() - t0 < seconds:
            await asyncio.sleep(0.05)
            for f in cam.raw_tap:
                rec.frame(f)
                age = link.clock.age_s(f.cap_us, f.recv_s) if link.clock.ready() else None
                if age is not None:
                    out.ages_ms.append(age * 1000.0)
                view.frame(f, age)
                out.pictures += 1
            cam.raw_tap.clear()
            while not link.samples.empty():
                view.telemetry(link.samples.get_nowait())
                out.samples += 1
    finally:
        stop.set()
        with contextlib.suppress(Exception):
            await link.close()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        rec.close()
        out.seconds = time.monotonic() - t0
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--ws-port", type=int, default=80)
    ap.add_argument("--stream-port", type=int, default=81)
    ap.add_argument("--seconds", type=float, default=None)
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--no-view", action="store_true")
    a = ap.parse_args()
    folder = a.out or Path("recordings") / time.strftime("%Y%m%d-%H%M%S")
    rec = SessionRecorder(folder, {"host": a.host, "what": "cq-record (Spec 7 phase 1)"})
    view: View = NoView()
    if not a.no_view:
        from conqueror_ai.adapters.rerun_view import RerunView

        view = RerunView(spawn=True, save=folder / "view.rrd")
    link = BridgeLink(a.host, a.ws_port)
    cam = MjpegSource(a.host, a.stream_port)
    print(f"recording to {folder} - Ctrl-C ends it (read only: drive with the phone)")
    try:
        summary = asyncio.run(record(link, cam, rec, view, a.seconds))
    except KeyboardInterrupt:
        summary = Summary()
        print("stopped")
    print(summary.text())
    print(f"{rec.frame_count} pictures and {rec.telem_count} telemetry messages in {folder}")


if __name__ == "__main__":
    main()
