"""`cq-map` (AI engine LLD §18.5, Plan Z4): recordings and the tank through Seeing and the room map."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app import map as app
from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.depth import DepthSettings, MapConfig
from conqueror_ai.domain.types import Frame
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
