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
    assert out.pictures >= 15 and out.samples >= 40
    assert out.ages_ms and 50 < sorted(out.ages_ms)[len(out.ages_ms) // 2] < 250  # ~100 ms + local delay
    jpegs = [f.jpeg for f in read_frames(tmp_path / "s1" / "frames.bin")]
    assert jpegs and all(j.startswith(b"\xff\xd8") and j.endswith(b"\xff\xd9") for j in jpegs)  # whole pictures
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
    rec.telemetry(2.0, json.dumps({"t": "pong"}))
    rec.frame(Frame(1, 10, 1.5, b"\xff\xd8\xff\xd9"))
    rec.telemetry(1.0, json.dumps({"t": "welcome"}))
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
