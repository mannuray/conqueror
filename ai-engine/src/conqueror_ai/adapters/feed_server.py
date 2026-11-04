"""The AI feed for /ai pages (AI engine LLD §22.1, §22.5): one WebSocket on the laptop,
ws://0.0.0.0:8770/feed, that up to four pages open. Each page has an outbox that keeps only the newest of
each kind (frames first), so a slow phone gets the newest picture, never a growing queue, and a page whose
send stalls for 5 s is closed. Nothing here can drive the tank: STOP and the programs go from the page to
the tank."""

from __future__ import annotations

import asyncio
import contextlib
import json
from collections.abc import AsyncGenerator, AsyncIterator, Callable
from typing import Any, Protocol

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

PROTO = 1
PATH = "/feed"
FRAME = "frame"  # the outbox slot of the binary frames
TRY_AGAIN = 1013  # a fifth page: "try again later"


class PageConn(Protocol):
    async def send(self, message: str | bytes, /) -> None: ...

    async def close(self, code: int = 1000, reason: str = "") -> None: ...

    def __aiter__(self) -> AsyncIterator[str | bytes]: ...


class _Page:
    def __init__(self, n: int, conn: PageConn) -> None:
        self.n, self.conn = n, conn
        self.slots: dict[str, str | bytes] = {}  # one per kind, newest wins; insertion order = send order
        self.wake = asyncio.Event()

    def put(self, kind: str, data: str | bytes) -> None:
        self.slots[kind] = data
        self.wake.set()

    def take(self) -> str | bytes | None:
        """The small text kinds first, the frame last (final review I1): a page too slow for every frame
        still gets the status and a teach reply - only frames are skipped."""
        if not self.slots:
            return None
        text = [k for k in self.slots if k != FRAME]
        return self.slots.pop(text[0] if text else FRAME)


class FeedServer:
    def __init__(
        self,
        host: str = "0.0.0.0",
        port: int = 8770,
        *,
        max_pages: int = 4,
        stall_s: float = 5.0,
        greeting: Callable[[], list[dict[str, object]]] = list,
    ) -> None:
        """greeting: what a page gets right after the hello (the status, why, things)."""
        self.host, self.port, self.max_pages, self.stall_s = host, port, max_pages, stall_s
        self.greeting = greeting
        self.bound_port = 0  # the port really listened on (port 0: the system's pick), once run() is up
        self._pages: dict[int, _Page] = {}
        self._next = 0
        self._requests: asyncio.Queue[tuple[int, dict[str, Any]]] = asyncio.Queue()
        self._opening = 0
        self._closing: set[asyncio.Task[None]] = set()

    @property
    def pages(self) -> int:
        return len(self._pages)

    async def run(self, stop: asyncio.Event) -> None:
        async with serve(self._handler, self.host, self.port, max_size=2**16) as srv:
            self.bound_port = int(next(iter(srv.sockets)).getsockname()[1])
            await stop.wait()

    async def _handler(self, ws: ServerConnection) -> None:
        if ws.request is None or ws.request.path != PATH:
            await ws.close(1008, "the AI feed is /feed")
            return
        await self.attach(ws)

    async def attach(self, conn: PageConn) -> None:
        """One page's life: hello and greeting, its writer, and its requests until it closes."""
        if len(self._pages) + self._opening >= self.max_pages:
            await conn.close(TRY_AGAIN, "four pages already")
            return
        self._next += 1
        page = _Page(self._next, conn)
        writer: asyncio.Task[None] | None = None
        self._opening += 1  # the place is taken before the greeting is awaited (final review M2)
        try:
            try:
                for msg in [{"t": "hello", "proto": PROTO}, *self.greeting()]:
                    await asyncio.wait_for(conn.send(json.dumps(msg)), self.stall_s)
            finally:
                self._opening -= 1
            self._pages[page.n] = page
            writer = asyncio.create_task(self._write(page))
            async for raw in conn:
                msg = _load(raw)
                if msg is not None:
                    self._requests.put_nowait((page.n, msg))
        except (ConnectionClosed, TimeoutError):
            pass
        finally:
            self._pages.pop(page.n, None)
            if writer is not None:
                writer.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await writer

    async def _write(self, page: _Page) -> None:
        while True:
            await page.wake.wait()
            page.wake.clear()
            while (data := page.take()) is not None:
                try:
                    await asyncio.wait_for(page.conn.send(data), self.stall_s)
                except TimeoutError:  # a stalled page: closed, its reader then ends
                    self._pages.pop(page.n, None)
                    # the close runs to its own end in the background (websockets aborts the socket after its
                    # close timeout) - never cut short here (final review M1)
                    closing = asyncio.create_task(_close(page.conn))
                    self._closing.add(closing)
                    closing.add_done_callback(self._closing.discard)
                    return
                except ConnectionClosed:
                    return

    def frame(self, data: bytes) -> None:
        for p in self._pages.values():
            p.put(FRAME, data)

    def send(self, msg: dict[str, object]) -> None:
        text = json.dumps(msg)
        for p in self._pages.values():
            p.put(str(msg["t"]), text)

    def reply(self, page: int, msg: dict[str, object]) -> None:
        p = self._pages.get(page)
        if p is not None:
            p.put(str(msg["t"]), json.dumps(msg))

    async def requests(self) -> AsyncGenerator[tuple[int, dict[str, Any]]]:
        while True:
            yield await self._requests.get()


async def _close(conn: PageConn) -> None:
    with contextlib.suppress(Exception):
        await conn.close(1001, "too slow")


def _load(raw: str | bytes) -> dict[str, Any] | None:
    try:
        m = json.loads(raw)
    except ValueError:
        return None
    return m if isinstance(m, dict) else None  # pyright: ignore[reportUnknownVariableType]
