"""The bridge's camera stream (`:81/stream`) as Frames, newest first (AI engine LLD §4: latest frame wins).

The stream allows two viewers (the phone + the engine). A picture that arrives while the last one is not yet
taken replaces it; nothing queues.
"""

from __future__ import annotations

import asyncio
import time

from conqueror_ai.domain.http import Dechunker
from conqueror_ai.domain.mjpeg import MjpegParser
from conqueror_ai.domain.types import Frame


class MjpegSource:
    def __init__(self, host: str, port: int = 81) -> None:
        self.host, self.port = host, port
        self.newest: Frame | None = None
        self.arrived = asyncio.Event()
        self.frames_in = 0
        self.raw_tap: list[Frame] | None = None  # the recorder takes every picture, not only the newest

    async def run(self, stop: asyncio.Event) -> None:
        delay = 0.5
        while not stop.is_set():
            try:
                await self._session(stop)
                delay = 0.5
            except (OSError, TimeoutError, ConnectionError, ValueError, asyncio.IncompleteReadError) as e:
                print(f"camera stream: {type(e).__name__} - trying again in {delay:.1f} s")
            if not stop.is_set():
                await asyncio.sleep(delay)
                delay = min(delay * 2, 5.0)

    async def _session(self, stop: asyncio.Event) -> None:
        reader, writer = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), 5)
        try:
            writer.write(f"GET /stream HTTP/1.1\r\nHost: {self.host}\r\nConnection: close\r\n\r\n".encode())
            await writer.drain()
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 5)
            status = head.split(b"\r\n", 1)[0]
            if b" 200 " not in status:
                raise ConnectionError(f"stream answered {status.decode('latin-1').strip()}")
            # the bridge (esp_http_server) always sends chunked; a plain stream is read as it is
            dechunk = Dechunker() if b"transfer-encoding: chunked" in head.lower() else None
            parser = MjpegParser()
            while not stop.is_set():
                data = await asyncio.wait_for(reader.read(65536), 5)
                if not data:
                    return
                now = time.monotonic()
                body = dechunk.feed(data) if dechunk is not None else data
                for part in parser.feed(body):
                    self.take(Frame(part.seq, part.cap_us, now, part.jpeg))
                if dechunk is not None and dechunk.done:
                    return
        finally:
            writer.close()

    def take(self, f: Frame) -> None:
        self.frames_in += 1
        self.newest = f
        if self.raw_tap is not None:
            self.raw_tap.append(f)
        self.arrived.set()

    async def next_frame(self) -> Frame:
        """Waits for a picture newer than the last one taken."""
        await self.arrived.wait()
        self.arrived.clear()
        f = self.newest
        assert f is not None
        return f
