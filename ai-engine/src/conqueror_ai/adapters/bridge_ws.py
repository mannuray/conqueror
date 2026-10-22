"""The bridge's WebSocket (AI engine LLD §6): `hello`, the 50 Hz debug stream, pings for the clock, and one
command - `cam_set`, the camera to exact angles (bridge LLD R22).

It never sends anything that moves the tank. Raw messages go to an optional tap (the recorder).
"""

from __future__ import annotations

import asyncio
import json
import time
from collections.abc import Callable
from typing import Any

from websockets.asyncio.client import ClientConnection, connect

from conqueror_ai.domain.clock import BridgeClock
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import TelemSample

RawTap = Callable[[float, str], None]  # (laptop receive time, raw JSON text)


class CommandRefused(Exception):
    """The bridge answered a command with a nack: `reason` (OUT_OF_RANGE, NOT_OWNER, BUSY, ...) + message."""

    def __init__(self, reason: str, message: str) -> None:
        super().__init__(f"{reason}: {message}")
        self.reason = reason


class BridgeLink:
    def __init__(self, host: str, port: int = 80, client: str = "ai-map", silent_s: float = 3.0) -> None:
        self.url = f"ws://{host}:{port}/ws"
        self.client = client
        self.silent_s = silent_s  # no telemetry for this long: open the link again (WiFi trouble)
        self.clock = BridgeClock()
        self.samples: asyncio.Queue[TelemSample] = asyncio.Queue(maxsize=500)
        self.tap: RawTap | None = None
        self.counts_per_m: int | None = None
        self.last_sample_s: float | None = None
        self._ws: ClientConnection | None = None
        self._next_id = 0
        self._waiting: dict[int, asyncio.Future[dict[str, Any]]] = {}

    @property
    def connected(self) -> bool:
        return self._ws is not None

    async def cam_set(self, pan_deg: float, tilt_deg: float, timeout_s: float = 3.0) -> None:
        """The camera to (pan, tilt) degrees from the centre, right / up = + (bridge LLD R22).

        Returns on the ack. Raises CommandRefused on a nack (OUT_OF_RANGE: pan -90..90, tilt -20..60),
        ConnectionError when not connected, TimeoutError without an answer. A camera angle is not motion:
        the engine still never moves the tank."""
        ws = self._ws
        if ws is None:
            raise ConnectionError("not connected to the bridge")
        self._next_id += 1
        mid = self._next_id
        reply: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
        self._waiting[mid] = reply
        try:
            await self._send(
                ws, {"t": "cam_set", "pan": round(pan_deg, 1), "tilt": round(tilt_deg, 1), "id": mid}
            )
            m = await asyncio.wait_for(reply, timeout_s)
        finally:
            self._waiting.pop(mid, None)
        if m.get("t") == "nack":
            raise CommandRefused(str(m.get("reason", "")), str(m.get("message", "")))

    async def run(self, stop: asyncio.Event) -> None:
        """Connects (again after a drop, with back-off) until `stop` is set."""
        delay = 0.5
        while not stop.is_set():
            try:
                async with connect(self.url, open_timeout=5, ping_interval=None) as ws:
                    self._ws = ws
                    delay = 0.5
                    await self._session(ws, stop)
            except (OSError, TimeoutError, ConnectionError) as e:
                print(f"bridge link: {type(e).__name__} - trying again in {delay:.1f} s")
            except Exception as e:  # websockets' ConnectionClosed and friends
                print(f"bridge link closed: {type(e).__name__} - trying again in {delay:.1f} s")
            finally:
                self._ws = None
                self.new_session()  # the bridge may be restarting: its clock will start from zero
            if not stop.is_set():
                await asyncio.sleep(delay)
                delay = min(delay * 2, 5.0)

    def new_session(self) -> None:
        """A new connection may be a restarted bridge, whose clock starts from zero again."""
        self.clock = BridgeClock()

    async def _session(self, ws: ClientConnection, stop: asyncio.Event) -> None:
        self.new_session()
        await self._send(ws, {"t": "hello", "client": self.client, "proto": 1})
        await self._send(ws, {"t": "debug", "on": True, "hz": 50})
        await self._send(ws, {"t": "calib"})
        self.last_sample_s = time.monotonic()  # the watchdog counts from the connection
        helpers = [asyncio.create_task(self._pings(ws, stop)), asyncio.create_task(self._watchdog(ws))]
        try:
            async for raw in ws:
                if stop.is_set():
                    break
                self._on_text(raw if isinstance(raw, str) else raw.decode("utf-8", "replace"))
        finally:
            for h in helpers:
                h.cancel()
            await asyncio.gather(
                *helpers, return_exceptions=True
            )  # their errors are collected, not left behind

    async def _watchdog(self, ws: ClientConnection) -> None:
        """Closes a link whose telemetry stopped while the socket stays open; run() then opens it again."""
        while True:
            await asyncio.sleep(min(0.2, self.silent_s / 2))
            if self.last_sample_s is not None and time.monotonic() - self.last_sample_s > self.silent_s:
                print(f"bridge link: no telemetry for {self.silent_s:.1f} s - opening it again")
                await ws.close()
                return

    async def _pings(self, ws: ClientConnection, stop: asyncio.Event) -> None:
        while not stop.is_set():  # also keeps the link alive: the bridge drops a client silent for 3 s
            await self._send(ws, {"t": "ping", "t0": round(time.monotonic() * 1000, 3)})
            await asyncio.sleep(1.0)

    def _on_text(self, raw: str) -> None:
        now = time.monotonic()
        if self.tap is not None:
            self.tap(now, raw)
        try:
            m: Any = json.loads(raw)
        except ValueError:
            return
        if not isinstance(m, dict):
            return
        self.on_message(m, now)  # pyright: ignore[reportUnknownArgumentType]

    def on_message(self, m: dict[str, Any], now: float) -> None:
        kind = m.get("t")
        if kind == "td":
            s = parse_td(m)
            if s is not None:
                self.last_sample_s = now
                if self.samples.full():
                    self.samples.get_nowait()  # the newest matter most
                self.samples.put_nowait(s)
        elif kind == "pong":
            t0, us = m.get("t0"), m.get("bridge_us")
            if isinstance(t0, int | float) and isinstance(us, int):
                self.clock.add(float(t0) / 1000.0, now, us)
        elif kind in ("ack", "nack"):
            mid = m.get("id")
            fut = self._waiting.get(mid) if isinstance(mid, int) else None
            if fut is not None and not fut.done():
                fut.set_result(m)
        elif kind == "calib":
            cpm = m.get("counts_per_m")
            if isinstance(cpm, int):
                self.counts_per_m = cpm

    async def _send(self, ws: ClientConnection, msg: dict[str, Any]) -> None:
        await ws.send(json.dumps(msg))

    async def close(self) -> None:
        ws = self._ws
        if ws is not None:
            try:
                await self._send(ws, {"t": "debug", "on": False})
            finally:
                await ws.close()
