"""`cq-pose`: a recorded session through the pose filter, for the floor check (AI engine LLD §14.5)."""

from __future__ import annotations

import json
import math
from pathlib import Path

import pytest

from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app import pose_replay


def td(t_ms: int, enc: tuple[int, int], pwm: tuple[int, int], yaw: float) -> str:
    return json.dumps(
        {
            "t": "td",
            "seq": t_ms,
            "uno_ms": t_ms,
            "bus_us": t_ms * 1000,
            "enc": list(enc),
            "pwm": list(pwm),
            "cmd": [0, 0],
            "yaw": yaw % 360.0,
            "rate": 0.0,
            "dist_mm": None,
            "pan": 90.0,
            "tilt": 90.0,
            "flags": 0,
        }
    )


def out_and_back(folder: Path, with_calib: bool = True) -> None:
    """1 m out, a 180 deg pivot to the left, 1 m back."""
    rec = SessionRecorder(folder, {"what": "test"})
    if with_calib:
        rec.telemetry(
            0.0, json.dumps({"t": "calib", "counts_per_m": 7514, "roll_mm": 27, "gyro_scale": 10310})
        )
    t, cl, r, yaw = 0, 0, 0, 0.0
    for _ in range(100):  # out: 7514 counts in 100 steps
        rec.telemetry(t / 1000, td(t, (cl, r), (120, 120), yaw))
        t, cl, r = t + 20, cl + 75, r + 75
    cl, r = cl + 14, r + 14  # the last 14 counts, so it is exactly 1 m
    for _ in range(90):  # pivot left, 2 deg per step
        rec.telemetry(t / 1000, td(t, (cl, r), (120, -120), yaw))
        t, cl, r, yaw = t + 20, cl + 30, r + 30, yaw + 2.0
    for _ in range(101):  # back
        rec.telemetry(t / 1000, td(t, (cl, r), (120, 120), yaw))
        t, cl, r = t + 20, cl + 75, r + 75
    rec.close()


def test_an_out_and_back_ends_near_the_start(tmp_path: Path) -> None:
    out_and_back(tmp_path / "s")
    s = pose_replay.summarize(tmp_path / "s")
    assert math.isclose(s.farthest_m, 1.0, abs_tol=0.002)
    assert math.hypot(s.end.x_m, s.end.y_m) < 0.01
    assert math.isclose(s.turned_deg, 180.0, abs_tol=0.5)
    assert math.isclose(s.driven_m, 2.0, abs_tol=0.02)
    assert s.counts_per_m == 7514 and s.samples == 291


def test_a_recording_without_counts_per_m_is_refused(tmp_path: Path) -> None:
    out_and_back(tmp_path / "s", with_calib=False)
    with pytest.raises(pose_replay.NoCalibration):
        pose_replay.summarize(tmp_path / "s")


def test_the_summary_reads_plainly(tmp_path: Path) -> None:
    out_and_back(tmp_path / "s")
    text = pose_replay.report(pose_replay.summarize(tmp_path / "s"))
    assert "end" in text and "farthest" in text and "turned" in text and "cm" in text
