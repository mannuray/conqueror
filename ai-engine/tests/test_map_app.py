"""`cq-map` (AI engine LLD §18.5, Plan Z4): recordings and the tank through Seeing and the room map."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path

import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app import map as app
from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.depth import DepthSettings, MapConfig
from conqueror_ai.domain.types import Frame
from tests.fake_bridge import FakeBridge
from tests.test_depth import inverse_depth
from tests.test_objects import CAL, cam_at
from tests.test_objects_app import GREY_JPEG

CFG = MapConfig(depth=DepthSettings(tilt_offset_deg=0.0))  # the fake depth has no offset


class FloorDepth:
    """A depth model that sees a bare floor from the test camera (15 deg down, at the start)."""

    def __init__(self) -> None:
        self.inv = inverse_depth(cam_at())

    def __call__(self, rgb: NDArray[np.uint8]) -> FloatArray:
        return self.inv


def td(t_ms: int, dist_mm: int | None) -> str:
    return json.dumps(
        {
            "t": "td",
            "seq": t_ms,
            "uno_ms": t_ms,
            "bus_us": t_ms * 1000,
            "enc": [0, 0],
            "pwm": [0, 0],
            "cmd": [0, 0],
            "yaw": 0.0,
            "rate": 0.0,
            "dist_mm": dist_mm,
            "pan": 90.0,
            "tilt": 90.0,
            "flags": 0,
        }
    )


def session(folder: Path, dist_mm: int | None = None) -> None:
    """2 s: the tank still, telemetry at 50 Hz with the given echo, a picture every 100 ms."""
    rec = SessionRecorder(folder, {"what": "test"})
    rec.telemetry(0.0, json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27, "gyro_scale": 10310}))
    for t in range(0, 2000, 20):
        rec.telemetry(t / 1000, td(t, dist_mm))
        if t % 100 == 0 and t >= 100:
            rec.frame(Frame(seq=t, cap_us=(t - 100) * 1000, recv_s=t / 1000, jpeg=GREY_JPEG))
    rec.close()


def test_replay_marks_floor_seen(tmp_path: Path) -> None:
    session(tmp_path / "s")
    run = app.run_replay(tmp_path / "s", CAL, CFG, FloorDepth())
    out = app.report(run)
    assert run.marked > 0 and "with marks" in out
    assert run.room.seen.sum() * run.room.cell_m**2 > 0.5
    assert not run.room.blocked().any()


def test_replay_echo_marks_low(tmp_path: Path) -> None:
    session(tmp_path / "s", dist_mm=800)
    run = app.run_replay(tmp_path / "s", CAL, CFG, FloorDepth())
    assert run.echoes > 0
    assert (run.room.low > CFG.map.blocked).any()
    assert not (run.room.high > CFG.map.blocked).any()
    assert "blocked low" in app.report(run)


def test_replay_without_depth(tmp_path: Path) -> None:  # Review Focus 4
    session(tmp_path / "s", dist_mm=800)
    run = app.run_replay(tmp_path / "s", CAL, CFG, None)
    out = app.report(run)
    assert run.depth_pictures == 0 and "with depth 0" in out
    assert not run.room.seen.any() and "seen 0.0 m²" in out
    assert run.echoes > 0 and (run.room.low > 0).any()


def test_replay_same_answer_twice(tmp_path: Path) -> None:
    session(tmp_path / "s", dist_mm=800)
    a = app.run_replay(tmp_path / "s", CAL, CFG, FloorDepth())
    b = app.run_replay(tmp_path / "s", CAL, CFG, FloorDepth())
    assert np.array_equal(a.room.low, b.room.low)
    assert np.array_equal(a.room.high, b.room.high)
    assert np.array_equal(a.room.seen, b.room.seen)


def test_no_echo_values_ignored(tmp_path: Path) -> None:
    session(tmp_path / "s", dist_mm=0)
    assert app.run_replay(tmp_path / "s", CAL, CFG, None).echoes == 0
    session(tmp_path / "t", dist_mm=None)
    assert app.run_replay(tmp_path / "t", CAL, CFG, None).echoes == 0


# ---- live (Task 5): the fake bridge, the camera straight ahead (servos at 90), real pictures
async def live(
    tmp_path: Path, seconds: float, rec: SessionRecorder | None = None, frames_for_s: float | None = None
) -> tuple[app.MapRun, FakeBridge]:
    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0), frames_for_s=frames_for_s)
    await bridge.start()
    try:
        link = BridgeLink("127.0.0.1", bridge.ws_port)
        cam = MjpegSource("127.0.0.1", bridge.stream_port)
        run = app.MapRun(CAL, CFG, FloorDepth())
        await app.run_live(link, cam, run, rec, None, seconds=seconds)
    finally:
        await bridge.stop()
    return run, bridge


async def test_live_fills_the_map(tmp_path: Path) -> None:
    run, _ = await live(tmp_path, 1.5)
    assert run.used > 0 and run.marked > 0
    assert run.room.seen.any()


async def test_live_records(tmp_path: Path) -> None:
    rec = SessionRecorder(tmp_path / "s", {"what": "test"})
    run, _ = await live(tmp_path, 1.5, rec)
    rec.close()
    again = app.run_replay(tmp_path / "s", CAL, CFG, FloorDepth())
    assert abs(again.used - run.used) <= 1
    assert again.room.seen.sum() >= 0.9 * run.room.seen.sum() > 0


async def test_live_never_drives(tmp_path: Path) -> None:
    _, bridge = await live(tmp_path, 1.0)
    sent = {m["t"] for m in bridge.received}
    assert not sent & {"drive", "turn", "move", "drive_rate", "logo_run", "stop", "cam_set"}


async def test_live_stale_reported(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    await live(tmp_path, 2.2, frames_for_s=0.3)
    out = capsys.readouterr().out
    assert "stale" in out and "no new picture" in out


async def test_live_ends_when_a_link_crashes(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    """Z4 review minor (deferred, now fixed): an unreachable tank said 'stale' for ever, never the error."""
    link = BridgeLink("127.0.0.1", 1)  # nothing listens: its task keeps retrying - the camera's fails
    cam = MjpegSource("127.0.0.1", 1)

    async def broken(stop: asyncio.Event) -> None:
        raise OSError("stream closed")

    cam.run = broken  # type: ignore[method-assign]
    await app.run_live(link, cam, app.MapRun(CAL, CFG, None), None, None, seconds=5.0)
    assert "failed" in capsys.readouterr().out
