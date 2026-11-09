"""`cq-person-test` (Spec 10 phase 0, Plan P0): record a crawling person at taped marks, then score them."""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path
from typing import cast

import cv2
import numpy as np
import pytest
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app.objects import CALIBRATION, VGA
from conqueror_ai.app.person_test import all_marks, record_marks, score_folder
from conqueror_ai.domain.camera import CameraModel, from_servo
from conqueror_ai.domain.depth import Seeing
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
            "127.0.0.1",
            bridge.ws_port,
            bridge.stream_port,
            tmp_path / "p0",
            marks,
            ask,
            still_s=0.8,
            countdown_s=0,
        )
    finally:
        await bridge.stop()
    assert done == ["1.0-0-still-t0", "1.0-0-still-t-15"]
    for mid in done:
        got = json.loads((tmp_path / "p0" / mid / "mark.json").read_text())
        assert got["mid"] == mid and got["complete"] is True
        assert (tmp_path / "p0" / mid / "frames.bin").stat().st_size > 0
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


def write_mark(
    folder: Path, mid: str, tape: float, still: bool, tilts: list[float], complete: bool = True, n: int = 30
) -> None:
    """A recorded mark: n pictures at 10 / s, telemetry before each one with the tilt servo from `tilts`."""
    rec = SessionRecorder(folder / mid, {"what": "test"})
    m = {
        "mid": mid,
        "tape_m": tape,
        "angle_deg": 0.0,
        "still": still,
        "tilt_deg": -15.0,
        "complete": complete,
    }
    (folder / mid / "mark.json").write_text(json.dumps(m))
    for i in range(n):
        t = 100.0 + i * 0.1
        rec.telemetry(t - 0.01, td(i, tilts[min(i, len(tilts) - 1)], int(tape * 1000) + 20))
        rec.frame(Frame(i, i * 100_000, t, VGA_JPEG))
    rec.close()


VGA_JPEG = bytes(cv2.imencode(".jpg", np.full((480, 640, 3), 128, dtype=np.uint8))[1].tobytes())


class PersonAt:
    """A faked detector: the person's hands d m ahead of the front edge on every picture; counts its calls."""

    def __init__(self, d: float) -> None:
        cal = calibration_store.load(CALIBRATION, VGA)
        cam = CameraModel(cal).for_frame(Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), *from_servo(90.0, 105.0))
        px = cam.floor_to_pixel(cal.track_front_m + d, 0.0)
        assert px is not None
        self.box = (px[0] - 40, px[1] - 120, px[0] + 40, px[1])
        self.calls = 0

    def __call__(self, rgb: NDArray[np.uint8], c: CameraModel | None) -> list[Detection]:
        self.calls += 1
        return [Detection("person", 0.9, self.box)]


def test_score_turns_the_recorded_boxes_into_the_bar(tmp_path: Path) -> None:
    """A recorded mark at 1.0 m: the faked detector puts the person's hands 1.0 m ahead on every picture."""
    cal = calibration_store.load(CALIBRATION, VGA)
    write_mark(tmp_path / "p0", "1.0-0-still-t-15", 1.0, True, [105.0])
    det = PersonAt(1.0)
    text = score_folder(tmp_path / "p0", cal, {"fake person": det}, depth=None)
    assert "1.0-0-still-t-15: pass  seen 10/10" in text
    assert text.splitlines()[-1] == "fake person: PASS (1 of 1 marks pass)"
    assert (tmp_path / "p0" / "report.txt").read_text() == text + "\n"


def test_still_pictures_are_spread_over_the_window_and_settle_after_a_camera_move(tmp_path: Path) -> None:
    """Review I3: the 10 still pictures span the window, not its first 0.4 s; review minor: a camera move
    (beyond jitter) restarts the 0.5 s settle; jitter under 0.5 degrees does not."""
    cal = calibration_store.load(CALIBRATION, VGA)
    tilts = [105.0] * 10 + [105.3] * 5 + [100.0] * 15  # jitter at 1.0 s, a real move at 1.5 s
    write_mark(tmp_path / "p0", "1.0-0-still-t-15", 1.0, True, tilts)

    from conqueror_ai.app import person_test as pt

    kept = pt.kept_frames(tmp_path / "p0" / "1.0-0-still-t-15", cal, None, None)
    caps = [f.cap_us // 100_000 for f, _ in kept]
    assert caps[0] == 5 and 6 <= caps[1] <= 9  # after the first 0.5 s; the jitter at 10 does not restart it
    assert all(not (15 <= c < 20) for c in caps)  # 0.5 s after the real move at 15 are dropped
    picked = pt.spread(len(kept), 10)
    assert picked[0] == 0 and picked[-1] == len(kept) - 1 and len(picked) == 10


def test_each_mark_gets_its_own_tilt_fix(tmp_path: Path) -> None:
    """Review I1: a fresh Seeing per mark - the fix never carries from one mark (or tilt) to the next."""
    cal = calibration_store.load(CALIBRATION, VGA)
    write_mark(tmp_path / "p0", "1.0-0-still-t-15", 1.0, True, [105.0])
    write_mark(tmp_path / "p0", "1.5-0-still-t-15", 1.5, True, [105.0])
    made: list[object] = []

    class FakeSeeing:
        def __init__(self) -> None:
            made.append(self)
            self.seen: set[int] = set()

        def angle_only(self, cam: CameraModel, inv: object, t_us: int) -> object:
            assert t_us not in self.seen  # one mark's pictures only, each once
            self.seen.add(t_us)

            class R:
                class fix:
                    tilt_fix_deg = 0.0

            return R()

    score_folder(
        tmp_path / "p0",
        cal,
        {"fake": PersonAt(1.0)},
        depth=lambda rgb: np.zeros((2, 2)),
        make_seeing=cast("Callable[[], Seeing]", FakeSeeing),
    )
    assert len(made) == 2


def test_a_mark_cut_short_is_flagged_and_a_wrong_picture_size_is_refused(tmp_path: Path) -> None:
    cal = calibration_store.load(CALIBRATION, VGA)
    write_mark(tmp_path / "p0", "1.0-0-still-t-15", 1.0, True, [105.0], complete=False)
    text = score_folder(tmp_path / "p0", cal, {"fake": PersonAt(1.0)}, depth=None)
    assert "1.0-0-still-t-15 (cut short)" in text
    rec = SessionRecorder(tmp_path / "q" / "m", {"what": "test"})
    m = {"mid": "m", "tape_m": 1.0, "angle_deg": 0.0, "still": True, "tilt_deg": -15.0}
    (tmp_path / "q" / "m" / "mark.json").write_text(json.dumps(m))
    rec.telemetry(99.0, td(0, 105.0, 1000))
    rec.frame(Frame(0, 0, 100.0, GREY_JPEG))  # QVGA, the calibration is VGA
    rec.close()
    with pytest.raises(ValueError, match="320 x 240"):
        score_folder(tmp_path / "q", cal, {"fake": PersonAt(1.0)}, depth=None)
