"""`cq-record`: watch the tank - camera and telemetry - record the session and show it live (Spec 7 phase 1).

Read only: it never moves the tank (drive the tank with the phone meanwhile). Ctrl-C ends it and prints a
summary: pictures and telemetry per second, and the pictures' age on arrival.

    uv run cq-record --host 192.168.31.167 [--seconds 60] [--no-view] [--out recordings/<now>]
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import signal
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


@dataclass
class Batch:
    """What arrived since the last save: taken on the event loop, saved and shown in a worker thread."""

    texts: list[tuple[float, str]] = field(default_factory=list[tuple[float, str]])
    frames: list[tuple[Frame, float | None]] = field(default_factory=list[tuple[Frame, float | None]])
    samples: list[TelemSample] = field(default_factory=list[TelemSample])


class Ages:
    """A picture's age on arrival, by the bridge clock - or None when it cannot be trusted.

    A capture time that jumps back by more than 1 s means the bridge restarted (its clock starts at
    zero): the old clock is dropped at once, even when the camera is back before the WebSocket
    notices (owner's tank, 2026-10-20: 12.7 s ages)."""

    MAX_S = 5.0

    def __init__(self) -> None:
        self._last_cap_us: int | None = None

    def of(self, link: BridgeLink, f: Frame) -> float | None:
        if self._last_cap_us is not None and f.cap_us < self._last_cap_us - 1_000_000:
            link.new_session()
        self._last_cap_us = f.cap_us
        if not link.clock.ready():
            return None
        age = link.clock.age_s(f.cap_us, f.recv_s)
        return age if 0.0 <= age <= self.MAX_S else None


def _take(link: BridgeLink, cam: MjpegSource, texts: list[tuple[float, str]], ages: Ages) -> Batch:
    """On the event loop: moves out everything that arrived (ages computed here, where the clock lives)."""
    b = Batch(texts=texts[:])
    texts.clear()
    for f in cam.raw_tap or []:
        b.frames.append((f, ages.of(link, f)))
    if cam.raw_tap is not None:
        cam.raw_tap.clear()
    while not link.samples.empty():
        b.samples.append(link.samples.get_nowait())
    return b


def _save(b: Batch, rec: SessionRecorder, view: View, out: Summary) -> None:
    """In a worker thread: file writes and drawing never hold up the WebSocket."""
    for t, raw in b.texts:
        rec.telemetry(t, raw)
    for f, age in b.frames:
        rec.frame(f)
        if age is not None:
            out.ages_ms.append(age * 1000.0)
        view.frame(f, age)
        out.pictures += 1
    for s in b.samples:
        view.telemetry(s)
        out.samples += 1


async def record(
    link: BridgeLink,
    cam: MjpegSource,
    rec: SessionRecorder,
    view: View,
    seconds: float | None,
    stop: asyncio.Event | None = None,
) -> Summary:
    """Records until `seconds` have passed or `stop` is set (cq-record sets it on Ctrl-C)."""
    halt = asyncio.Event()
    asked = stop or asyncio.Event()
    texts: list[tuple[float, str]] = []
    ages = Ages()
    link.tap = lambda t, raw: texts.append((t, raw))
    cam.raw_tap = []
    tasks = [asyncio.create_task(link.run(halt)), asyncio.create_task(cam.run(halt))]
    out = Summary()
    t0 = time.monotonic()
    try:
        while not asked.is_set() and (seconds is None or time.monotonic() - t0 < seconds):
            await asyncio.sleep(0.05)
            await asyncio.to_thread(_save, _take(link, cam, texts, ages), rec, view, out)
    finally:
        halt.set()
        with contextlib.suppress(Exception):
            await link.close()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        # what arrived in the last moment is kept too, also off the loop
        # (cq-record ends by `stop`, never by cancelling this task)
        await asyncio.to_thread(_save, _take(link, cam, texts, ages), rec, view, out)
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

    async def until_ctrl_c() -> Summary:
        stop = asyncio.Event()
        asyncio.get_running_loop().add_signal_handler(signal.SIGINT, stop.set)  # Ctrl-C ends it cleanly
        return await record(link, cam, rec, view, a.seconds, stop)

    summary = asyncio.run(until_ctrl_c())
    print(summary.text())
    print(f"{rec.frame_count} pictures and {rec.telem_count} telemetry messages in {folder}")


if __name__ == "__main__":
    main()
