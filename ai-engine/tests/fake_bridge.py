"""A stand-in bridge for tests: the WebSocket (`welcome`, `td` at 50 Hz after `debug`,
`pong` with `bridge_us`, `calib`) and the camera stream (parts with `X-Frame-Us` / `X-Frame-Seq`),
on local ports, with the bridge clock running 1000 s ahead of the laptop's monotonic clock."""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
from typing import Any

from websockets.asyncio.server import Server, ServerConnection, serve

OFFSET_S = 1000.0
JPEG = b"\xff\xd8" + b"\x00" * 200 + b"\xff\xd9"


def bridge_us() -> int:
    return int((time.monotonic() + OFFSET_S) * 1e6)


class FakeBridge:
    def __init__(self, frame_age_s: float = 0.1, fps: float = 20.0) -> None:
        self.frame_age_s = frame_age_s
        self.fps = fps
        self.received: list[dict[str, Any]] = []
        self.ws_port = 0
        self.stream_port = 0
        self._ws_server: Server | None = None
        self._stream_server: asyncio.Server | None = None

    async def start(self) -> None:
        self._ws_server = await serve(self._ws, "127.0.0.1", 0)
        self.ws_port = next(iter(self._ws_server.sockets)).getsockname()[1]
        self._stream_server = await asyncio.start_server(self._stream, "127.0.0.1", 0)
        self.stream_port = self._stream_server.sockets[0].getsockname()[1]

    async def stop(self) -> None:
        if self._ws_server is not None:
            self._ws_server.close()
            await self._ws_server.wait_closed()
        if self._stream_server is not None:
            self._stream_server.close()
            with contextlib.suppress(Exception):
                await asyncio.wait_for(self._stream_server.wait_closed(), 1)

    async def _ws(self, ws: ServerConnection) -> None:
        sender: asyncio.Task[None] | None = None
        try:
            async for raw in ws:
                m = json.loads(raw)
                self.received.append(m)
                t = m.get("t")
                if t == "hello":
                    await ws.send(json.dumps({"t": "welcome", "you": "ai-1"}))
                elif t == "debug" and m.get("on") and sender is None:
                    sender = asyncio.create_task(self._td(ws))
                elif t == "ping":
                    await ws.send(
                        json.dumps({"t": "pong", "t0": m["t0"], "tank_ms": 1, "bridge_us": bridge_us()})
                    )
                elif t == "calib":
                    await ws.send(json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27}))
        finally:
            if sender is not None:
                sender.cancel()

    async def _td(self, ws: ServerConnection) -> None:
        seq = 0
        while True:
            seq += 1
            await ws.send(
                json.dumps(
                    {
                        "t": "td",
                        "seq": seq,
                        "uno_ms": seq * 20,
                        "bus_us": bridge_us(),
                        "enc": [seq, seq],
                        "pwm": [0, 0],
                        "cmd": [0, 0],
                        "act": [0, 0],
                        "yaw": 1.5,
                        "rate": 0.1,
                        "pitch": 0.0,
                        "roll": 0.0,
                        "dist_mm": 500,
                        "closing": 0,
                        "pan": 0.0,
                        "tilt": 0.0,
                        "flags": 0,
                        "prim": 0,
                        "pct": 0,
                    }
                )
            )
            await asyncio.sleep(0.02)

    async def _stream(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        await reader.readuntil(b"\r\n\r\n")
        writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace;boundary=cqframe\r\n\r\n")
        seq = 0
        try:
            while True:
                seq += 1
                cap = bridge_us() - int(self.frame_age_s * 1e6)  # taken frame_age_s ago
                head = (
                    b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n"
                    b"X-Frame-Us: %d\r\nX-Frame-Seq: %d\r\n\r\n" % (len(JPEG), cap, seq)
                )
                writer.write(head + JPEG)
                await writer.drain()
                await asyncio.sleep(1.0 / self.fps)
        except (ConnectionError, asyncio.CancelledError):
            pass
        finally:
            writer.close()
