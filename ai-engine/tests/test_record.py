from __future__ import annotations

import json
from pathlib import Path

import pytest

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder, read_frames, replay
from conqueror_ai.app.record import NoView, Summary, record
from conqueror_ai.app.replay import counts
from conqueror_ai.domain.types import Frame
from tests.fake_bridge import FakeBridge


async def test_record_a_session_from_the_bridge_and_replay_it(tmp_path: Path) -> None:
    bridge = FakeBridge(frame_age_s=0.1, fps=20)
    await bridge.start()
    try:
        link = BridgeLink("127.0.0.1", bridge.ws_port)
        cam = MjpegSource("127.0.0.1", bridge.stream_port)
        rec = SessionRecorder(tmp_path / "s1", {"what": "test"})
        out = await record(link, cam, rec, NoView(), seconds=1.5)
    finally:
        await bridge.stop()
    sent = [m["t"] for m in bridge.received]
    assert sent[:3] == ["hello", "debug", "calib"]
    assert "ping" in sent
    assert not set(sent) & {"drive", "turn", "move", "drive_rate", "logo_run", "stop"}  # read only
    assert link.counts_per_m == 7514
    # wide margins: ~30 pictures and ~75 samples are expected, a loaded laptop must not fail the test
    assert out.pictures >= 10 and out.samples >= 30
    assert out.ages_ms and 50 < sorted(out.ages_ms)[len(out.ages_ms) // 2] < 400  # ~100 ms + local delay
    jpegs = [f.jpeg for f in read_frames(tmp_path / "s1" / "frames.bin")]
    assert jpegs and all(
        j.startswith(b"\xff\xd8") and j.endswith(b"\xff\xd9") for j in jpegs
    )  # whole pictures
    pictures, samples = counts(tmp_path / "s1")
    assert (pictures, samples) == (rec.frame_count, out.samples)
    times = [e.recv_s for e in replay(tmp_path / "s1")]
    assert times == sorted(times)
    assert "pictures/s" in out.text()


def test_a_session_is_never_written_over(tmp_path: Path) -> None:
    SessionRecorder(tmp_path / "s", {}).close()
    with pytest.raises(FileExistsError):
        SessionRecorder(tmp_path / "s", {})


def test_a_cut_off_picture_at_the_end_is_dropped(tmp_path: Path) -> None:
    rec = SessionRecorder(tmp_path / "s", {})
    rec.frame(Frame(1, 10, 1.0, b"\xff\xd8AA\xff\xd9"))
    rec.frame(Frame(2, 20, 2.0, b"\xff\xd8BBBB\xff\xd9"))
    rec.close()
    data = (tmp_path / "s" / "frames.bin").read_bytes()
    (tmp_path / "s" / "frames.bin").write_bytes(data[:-3])
    assert [f.seq for f in read_frames(tmp_path / "s" / "frames.bin")] == [1]


def test_replay_order_and_telemetry_lines(tmp_path: Path) -> None:
    rec = SessionRecorder(tmp_path / "s", {})
    rec.telemetry(1.0, json.dumps({"t": "welcome"}))  # the recorder writes each file in arrival order
    rec.frame(Frame(1, 10, 1.5, b"\xff\xd8\xff\xd9"))
    rec.telemetry(2.0, json.dumps({"t": "pong"}))
    rec.close()
    events = list(replay(tmp_path / "s"))
    assert [e.recv_s for e in events] == [1.0, 1.5, 2.0]
    assert events[1].frame is not None and events[0].text is not None


def test_summary_without_clock() -> None:
    s = Summary(seconds=2.0, pictures=30, samples=100)
    assert "15.0 pictures/s" in s.text() and "unknown" in s.text()
    assert Summary().text() == "nothing recorded"


def test_the_live_view_logs_pictures_and_telemetry(tmp_path: Path) -> None:
    from conqueror_ai.adapters.rerun_view import RerunView
    from conqueror_ai.domain.types import TelemSample

    view = RerunView(spawn=False, save=tmp_path / "v.rrd")
    view.telemetry(TelemSample(1_000_000, 1, 2, 3, 4.5, 0.1, 500, 0.0, 0.0, 0, 0, 0))
    view.telemetry(TelemSample(1_020_000, 2, 3, 4, 4.6, 0.1, None, 0.0, 0.0, 0, 0, 0))
    view.frame(Frame(1, 1_010_000, 1.0, b"\xff\xd8\xff\xd9"), 0.12)
    import rerun as rr

    rr.disconnect()  # flush the file
    assert (tmp_path / "v.rrd").stat().st_size > 0


def test_a_new_connection_starts_a_fresh_clock() -> None:
    # a restarted bridge counts from zero again: old clock samples would give wrong ages for up to 60 s
    link = BridgeLink("127.0.0.1")
    link.on_message({"t": "pong", "t0": 1000.0, "bridge_us": 5_000_000_000}, 1.001)
    assert link.clock.ready()
    link.new_session()
    assert not link.clock.ready()


async def test_stopping_by_request_keeps_the_summary(tmp_path: Path) -> None:
    # Ctrl-C sets `stop` (a signal handler in cq-record): the summary and every picture so far are kept
    import asyncio

    bridge = FakeBridge()
    await bridge.start()
    stop = asyncio.Event()
    try:
        link = BridgeLink("127.0.0.1", bridge.ws_port)
        cam = MjpegSource("127.0.0.1", bridge.stream_port)
        rec = SessionRecorder(tmp_path / "s", {})
        asyncio.get_running_loop().call_later(1.0, stop.set)
        out = await record(link, cam, rec, NoView(), seconds=None, stop=stop)
    finally:
        await bridge.stop()
    assert out.pictures > 5 and out.seconds > 0.9
    assert out.pictures == rec.frame_count  # nothing left behind in the camera's tap


async def test_a_dropped_connection_forgets_the_clock() -> None:
    # a bridge that restarts counts from zero: the old offset must not age the new pictures
    import asyncio

    bridge = FakeBridge()
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    for _ in range(40):
        await asyncio.sleep(0.05)
        if link.clock.ready():
            break
    assert link.clock.ready()
    await bridge.stop()
    await asyncio.sleep(0.3)
    assert not link.clock.ready()
    stop.set()
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)


def test_a_recording_cut_inside_a_line_reads_up_to_the_cut(tmp_path: Path) -> None:
    rec = SessionRecorder(tmp_path / "s", {})
    rec.telemetry(1.0, json.dumps({"t": "td"}))
    rec.telemetry(2.0, json.dumps({"t": "pong"}))
    rec.frame(Frame(1, 10, 1.0, b"\xff\xd8AA\xff\xd9"))
    rec.frame(Frame(2, 20, 2.0, b"\xff\xd8BB\xff\xd9"))
    rec.close()
    tel = tmp_path / "s" / "telemetry.jsonl"
    tel.write_text(tel.read_text()[:-12])  # cut inside the last telemetry line
    frames = tmp_path / "s" / "frames.bin"
    data = frames.read_bytes()
    second = data.index(b'{"seq": 2')
    frames.write_bytes(data[: second + 8])  # cut inside the second picture's header line
    events = list(replay(tmp_path / "s"))
    assert [e.recv_s for e in events] == [1.0, 1.0]
    assert events[1].frame is not None and events[1].frame.seq == 1


async def test_a_silent_link_is_opened_again() -> None:
    # telemetry stops but the socket stays open (WiFi trouble): reconnect instead of waiting for TCP
    import asyncio

    bridge = FakeBridge(send_td=False)
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port, silent_s=0.4)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    await asyncio.sleep(1.8)
    stop.set()
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    await bridge.stop()
    assert bridge.connections >= 2


async def test_a_dropped_link_leaves_no_unretrieved_task_error() -> None:
    import asyncio
    import gc

    errors: list[str] = []
    loop = asyncio.get_running_loop()
    loop.set_exception_handler(lambda _loop, ctx: errors.append(str(ctx.get("message"))))
    bridge = FakeBridge()
    await bridge.start()
    link = BridgeLink("127.0.0.1", bridge.ws_port)
    stop = asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    await asyncio.sleep(1.3)  # at least one ping sent
    await bridge.stop()
    await asyncio.sleep(1.3)  # the pinger's next send fails on the closed socket
    stop.set()
    task.cancel()
    await asyncio.gather(task, return_exceptions=True)
    gc.collect()
    await asyncio.sleep(0)
    loop.set_exception_handler(None)
    assert not [e for e in errors if "never retrieved" in e]


def test_replay_reads_pictures_as_it_goes(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    # a long session must not be loaded into memory whole before the first event comes out
    from collections.abc import Iterator

    import conqueror_ai.adapters.session as session

    rec = SessionRecorder(tmp_path / "s", {})
    rec.telemetry(0.5, json.dumps({"t": "td"}))
    rec.close()

    def frames_then_fail(_path: Path) -> Iterator[Frame]:
        yield Frame(1, 10, 1.0, b"\xff\xd8\xff\xd9")
        raise RuntimeError("read too far ahead")

    monkeypatch.setattr(session, "read_frames", frames_then_fail)
    events = session.replay(tmp_path / "s")
    assert next(events).recv_s == 0.5
    assert next(events).frame is not None


async def test_slow_saving_never_holds_up_the_link(tmp_path: Path) -> None:
    # writing pictures and drawing them happen off the event loop: a slow disk or viewer must not stall
    # the WebSocket (pings, telemetry) - measured as how late a 10 ms ticker wakes up
    import asyncio
    import time as _time

    from conqueror_ai.domain.types import TelemSample

    class SlowView:
        def telemetry(self, s: TelemSample) -> None:
            pass

        def frame(self, f: Frame, age_s: float | None) -> None:
            _time.sleep(0.15)

    bridge = FakeBridge(fps=10)
    await bridge.start()
    worst = 0.0
    done = asyncio.Event()

    async def ticker() -> None:
        nonlocal worst
        while not done.is_set():
            t = _time.monotonic()
            await asyncio.sleep(0.01)
            worst = max(worst, _time.monotonic() - t - 0.01)

    tick = asyncio.create_task(ticker())
    try:
        link = BridgeLink("127.0.0.1", bridge.ws_port)
        cam = MjpegSource("127.0.0.1", bridge.stream_port)
        rec = SessionRecorder(tmp_path / "s", {})
        out = await record(link, cam, rec, SlowView(), seconds=1.5)
    finally:
        done.set()
        await tick
        await bridge.stop()
    assert out.pictures > 0
    assert worst < 0.1, f"the event loop stalled {worst:.2f} s"
