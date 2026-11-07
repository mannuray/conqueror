"""FeedServer: the AI feed for /ai pages on port 8770 - newest wins per page, a stalled page is
closed (AI engine LLD §22.5). Most tests use a fake connection whose send can be held: a real localhost
socket buffers megabytes and would not stall within a test."""

from __future__ import annotations

import asyncio
import json
from collections.abc import AsyncIterator

from websockets.asyncio.client import connect

from conqueror_ai.adapters.feed_server import FeedServer


class FakePage:
    def __init__(self) -> None:
        self.got: list[str | bytes] = []
        self.gate = asyncio.Event()
        self.gate.set()
        self.closed: int | None = None
        self.incoming: asyncio.Queue[str | None] = asyncio.Queue()

    async def send(self, data: str | bytes) -> None:
        await self.gate.wait()
        self.got.append(data)

    async def close(self, code: int = 1000, reason: str = "") -> None:
        self.closed = code
        self.incoming.put_nowait(None)

    def __aiter__(self) -> AsyncIterator[str | bytes]:
        return self._read()

    async def _read(self) -> AsyncIterator[str | bytes]:
        while (m := await self.incoming.get()) is not None:
            yield m

    def texts(self) -> list[dict[str, object]]:
        return [json.loads(g) for g in self.got if isinstance(g, str)]


async def settle() -> None:
    for _ in range(5):
        await asyncio.sleep(0.01)


async def test_hello_then_greeting() -> None:
    srv = FeedServer(greeting=lambda: [{"t": "things", "list": []}, {"t": "why", "lines": []}])
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    assert [m["t"] for m in p.texts()] == ["hello", "things", "why"]
    assert p.texts()[0] == {"t": "hello", "proto": 1}
    assert srv.pages == 1
    await p.close()
    await task
    assert srv.pages == 0


async def test_frame_newest_wins_while_blocked() -> None:
    srv = FeedServer()
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    p.gate.clear()
    srv.frame(b"1")
    await settle()  # frame 1 is in flight, held at the gate
    srv.frame(b"2")
    srv.frame(b"3")
    p.gate.set()
    await settle()
    assert [g for g in p.got if isinstance(g, bytes)] == [b"1", b"3"]
    await p.close()
    await task


async def test_status_newest_wins_per_kind() -> None:
    srv = FeedServer()
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    p.gate.clear()
    srv.send({"t": "status", "n": 1})
    await settle()
    srv.send({"t": "status", "n": 2})
    srv.send({"t": "why", "lines": ["a"]})
    srv.send({"t": "status", "n": 3})
    p.gate.set()
    await settle()
    later = p.texts()[1:]
    assert [(m["t"], m.get("n")) for m in later] == [("status", 1), ("status", 3), ("why", None)]
    await p.close()
    await task


async def test_a_slow_page_still_gets_text() -> None:
    """Final review I1: frames come faster than a weak link sends - the status and a reply still get
    through."""
    srv = FeedServer()
    p = FakePage()

    async def slow(data: str | bytes) -> None:
        await asyncio.sleep(0.05)
        p.got.append(data)

    p.send = slow  # type: ignore[method-assign]
    task = asyncio.create_task(srv.attach(p))
    await settle()
    for i in range(20):  # a frame every 20 ms, a status every 100 ms, one reply
        srv.frame(bytes([i]))
        if i % 5 == 0:
            srv.send({"t": "status", "n": i})
        if i == 3:
            srv.reply(1, {"t": "rep", "id": 1})
        await asyncio.sleep(0.02)
    await asyncio.sleep(0.3)
    kinds = [m["t"] for m in p.texts()]
    assert "rep" in kinds and kinds.count("status") >= 2
    assert any(isinstance(g, bytes) for g in p.got)
    await p.close()
    await task


async def test_two_pages_at_once_never_make_five() -> None:
    """Final review M2: the place is taken before the greeting is sent."""
    srv = FeedServer()
    pages = [FakePage() for _ in range(6)]
    for p in pages:
        p.gate.clear()  # the greetings wait: all six are opening together
    tasks = [asyncio.create_task(srv.attach(p)) for p in pages]
    await settle()
    for p in pages:
        p.gate.set()
    await settle()
    assert srv.pages == 4 and sum(p.closed == 1013 for p in pages) == 2
    for p in pages:
        await p.close()
    await asyncio.gather(*tasks)


async def test_stalled_page_closed_after_stall_s() -> None:
    srv = FeedServer(stall_s=0.2)
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    p.gate.clear()
    srv.frame(b"big")
    await asyncio.wait_for(task, 2.0)
    await settle()
    assert p.closed == 1001  # the close runs to its end (final review M1: not cut short by a limit)
    assert srv.pages == 0


async def test_fifth_page_refused() -> None:
    srv = FeedServer()
    pages = [FakePage() for _ in range(5)]
    tasks = [asyncio.create_task(srv.attach(p)) for p in pages]
    await settle()
    assert srv.pages == 4
    assert pages[4].closed == 1013
    for p in pages[:4]:
        await p.close()
    await asyncio.gather(*tasks)


async def test_requests_yield_page_and_message() -> None:
    srv = FeedServer()
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    p.incoming.put_nowait("not json")
    p.incoming.put_nowait("[1, 2]")
    p.incoming.put_nowait('{"t":"teach","id":1,"word":"cup","names":["mug"]}')
    reqs = srv.requests()
    n, msg = await asyncio.wait_for(anext(reqs), 1.0)
    assert msg == {"t": "teach", "id": 1, "word": "cup", "names": ["mug"]}
    srv.reply(n, {"t": "rep", "id": 1, "ok": True, "msg": "I know cup now."})
    await settle()
    assert p.texts()[-1]["t"] == "rep"
    await p.close()
    await task
    await reqs.aclose()


async def test_no_pages_frame_is_free() -> None:
    srv = FeedServer()
    srv.frame(b"early")
    srv.send({"t": "status"})
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    assert not any(isinstance(g, bytes) for g in p.got)
    assert [m["t"] for m in p.texts()] == ["hello"]
    await p.close()
    await task


async def test_real_socket_round_trip() -> None:
    srv = FeedServer("127.0.0.1", 0)
    stop = asyncio.Event()
    run = asyncio.create_task(srv.run(stop))
    for _ in range(100):
        if srv.bound_port:
            break
        await asyncio.sleep(0.01)
    async with connect(f"ws://127.0.0.1:{srv.bound_port}/feed") as ws:
        assert json.loads(await ws.recv()) == {"t": "hello", "proto": 1}
        srv.frame(b"\x00\x00\x00\x02{}")
        assert await asyncio.wait_for(ws.recv(), 1.0) == b"\x00\x00\x00\x02{}"
    refused = False
    try:
        async with connect(f"ws://127.0.0.1:{srv.bound_port}/other") as ws:
            await asyncio.wait_for(ws.recv(), 1.0)
    except Exception:  # closed (or never opened): another path is not served
        refused = True
    assert refused
    stop.set()
    await asyncio.wait_for(run, 2.0)


async def test_room_on_off_per_page() -> None:
    """LLD §23.2: a page starts with room off; room on / off is kept per page and gone when it closes."""
    srv = FeedServer()
    a, b = FakePage(), FakePage()
    ta, tb = asyncio.create_task(srv.attach(a)), asyncio.create_task(srv.attach(b))
    await settle()
    assert srv.rooms() == set() and not srv.room_wanted()
    a.incoming.put_nowait(json.dumps({"t": "room", "on": True}))
    await settle()
    assert len(srv.rooms()) == 1 and srv.room_wanted()
    a.incoming.put_nowait(json.dumps({"t": "room", "on": False}))
    b.incoming.put_nowait(json.dumps({"t": "room", "on": True}))
    await settle()
    assert len(srv.rooms()) == 1
    await b.close()
    await tb
    assert srv.rooms() == set()
    await a.close()
    await ta


async def test_room_message_not_a_request() -> None:
    srv = FeedServer()
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    p.incoming.put_nowait(json.dumps({"t": "room", "on": True}))
    p.incoming.put_nowait(json.dumps({"t": "name", "id": 1, "x": 0, "y": 0, "name": "bed"}))
    await settle()
    reqs = srv.requests()
    _, first = await asyncio.wait_for(anext(reqs), 1)
    assert first["t"] == "name"
    await p.close()
    await task


async def test_room_frame_only_to_room_pages() -> None:
    srv = FeedServer()
    room, play = FakePage(), FakePage()
    tr, tp = asyncio.create_task(srv.attach(room)), asyncio.create_task(srv.attach(play))
    await settle()
    room.incoming.put_nowait(json.dumps({"t": "room", "on": True}))
    await settle()
    srv.room_frame(b"MAP")
    srv.send_rooms({"t": "room", "trail": []})
    await settle()
    assert b"MAP" in room.got and any(m["t"] == "room" for m in room.texts())
    assert b"MAP" not in play.got and not any(m["t"] == "room" for m in play.texts())
    for p in (room, play):
        await p.close()
    await asyncio.gather(tr, tp)


async def test_two_reps_both_sent() -> None:
    """A name answer never replaces a waiting teach answer: rep slots are kept by id."""
    srv = FeedServer()
    p = FakePage()
    task = asyncio.create_task(srv.attach(p))
    await settle()
    (n,) = srv._pages  # pyright: ignore[reportPrivateUsage]
    p.gate.clear()
    srv.reply(n, {"t": "rep", "id": 4, "ok": True, "msg": "I know cup now."})
    srv.reply(n, {"t": "rep", "id": 5, "ok": True, "msg": "Named it bed."})
    p.gate.set()
    await settle()
    assert sorted(int(m["id"]) for m in p.texts() if m["t"] == "rep") == [4, 5]  # pyright: ignore[reportArgumentType]
    await p.close()
    await task
