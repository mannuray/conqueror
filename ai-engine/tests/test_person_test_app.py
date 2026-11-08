"""`cq-person-test` (Spec 10 phase 0, Plan P0): record a crawling person at taped marks, then score them."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app.objects import CALIBRATION, VGA
from conqueror_ai.app.person_test import all_marks, record_marks, score_folder
from conqueror_ai.domain.camera import CameraModel, from_servo
from conqueror_ai.domain.person_test import Mark
from conqueror_ai.domain.types import Detection, Frame, Pose
from tests.fake_bridge import FakeBridge
from tests.test_objects_app import GREY_JPEG

MOVES = {"drive", "turn", "move", "drive_rate", "logo_run", "stop"}


def test_the_marks_go_tilt_by_tilt_so_the_camera_moves_twice() -> None:
    marks = all_marks()
    assert len(marks) == 40
    tilts = [m.tilt_deg for m in marks]
    assert tilts == sorted(tilts, reverse=True) and set(tilts) == {0.0, -15.0}  # level first, then driving
    assert {m.tape_m for m in marks} == {0.3, 0.5, 1.0, 1.5, 2.0}
    assert {m.angle_deg for m in marks} == {0.0, -30.0}
    assert len({m.mid for m in marks}) == 40


async def test_record_asks_mark_by_mark_skips_and_never_drives(tmp_path: Path) -> None:
    bridge = FakeBridge(jpeg=GREY_JPEG, pan_tilt=(90.0, 90.0))
    await bridge.start()
    marks = [
        Mark("1.0-0-still-t0", 1.0, 0.0, True, 0.0),
        Mark("1.5-0-still-t0", 1.5, 0.0, True, 0.0),
        Mark("1.0-0-still-t-15", 1.0, 0.0, True, -15.0),
    ]
    answers = iter(["", "", "s", "", ""])  # camera to level, mark 1, skip mark 2, camera down, mark 3
    said: list[str] = []

    def ask(prompt: str) -> str:
        said.append(prompt)
        return next(answers)

    try:
        done = await record_marks(
            "127.0.0.1", bridge.ws_port, bridge.stream_port, tmp_path / "p0", marks, ask, still_s=0.8
        )
    finally:
        await bridge.stop()
    assert done == ["1.0-0-still-t0", "1.0-0-still-t-15"]
    for mid in done:
        got = json.loads((tmp_path / "p0" / mid / "mark.json").read_text())
        assert got["mid"] == mid and (tmp_path / "p0" / mid / "frames.bin").stat().st_size > 0
    assert not (tmp_path / "p0" / "1.5-0-still-t0").exists()
    tilts = [m["tilt"] for m in bridge.received if m["t"] == "cam_set"]
    assert tilts == [0.0, -15.0]  # once per tilt, said before it moves
    assert any("only the camera moves" in s for s in said)
    assert not {m["t"] for m in bridge.received} & MOVES  # nothing drives


def td(t: int, tilt_servo: float, dist_mm: int) -> str:
    return json.dumps(
        {
            "t": "td", "seq": t, "uno_ms": t, "bus_us": t * 1000, "enc": [0, 0], "pwm": [0, 0],
            "cmd": [0, 0], "act": [0, 0], "yaw": 0.0, "rate": 0.0, "dist_mm": dist_mm, "pan": 90.0,
            "tilt": tilt_servo, "flags": 0,
        }
    )  # fmt: skip


def test_score_turns_the_recorded_boxes_into_the_bar(tmp_path: Path) -> None:
    """A recorded mark at 1.0 m: the faked detector puts the person's hands 1.0 m ahead on every picture."""
    cal = calibration_store.load(CALIBRATION, VGA)
    folder = tmp_path / "p0"
    mid = "1.0-0-still-t-15"
    rec = SessionRecorder(folder / mid, {"what": "test"})
    (folder / mid / "mark.json").write_text(json.dumps({"mid": mid, "tape_m": 1.0, "angle_deg": 0.0,
                                                        "still": True, "tilt_deg": -15.0}))  # fmt: skip
    for i in range(30):  # 3 s at 10 / s, telemetry before each picture
        t = 100.0 + i * 0.1
        rec.telemetry(t - 0.01, td(i, 105.0, 1050))
        rec.frame(Frame(i, i * 100_000, t, GREY_JPEG))
    rec.close()
    cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), *from_servo(90.0, 105.0))
    px = cam.floor_to_pixel(cal.track_front_m + 1.0, 0.0)
    assert px is not None

    def fake(rgb: NDArray[np.uint8], c: CameraModel | None) -> list[Detection]:
        return [Detection("person", 0.9, (px[0] - 40, px[1] - 120, px[0] + 40, px[1]))]

    text = score_folder(folder, cal, {"fake person": fake}, depth=None)
    assert f"{mid}: pass  seen 10/10" in text  # a still mark: the first 10 pictures after the first 0.5 s
    assert text.splitlines()[-1] == "fake person: PASS (1 of 1 marks pass)"
    assert (folder / "report.txt").read_text() == text + "\n"
