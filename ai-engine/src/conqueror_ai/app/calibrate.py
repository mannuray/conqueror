"""`cq-calibrate` - the calibration tool (AI engine LLD §8, design-notes §123). Steps, in order:

  lens    the checkerboard on the laptop screen, ~20 pictures -> focal length, centre, distortion
  mount   the measured mount, the laser level's horizon line -> the real tilt, then two tape marks to check
  servos  commanded -> real tilt (the horizon line) and pan (the level's vertical line, camera level)
  study   the blind zone and the view at a few tilts, for the owner's driving-tilt decision

Each step saves config/calibration.yaml. The tool's only command to the tank is cam_set (bridge LLD R22): it
never drives. The steps take their inputs as functions, so tests drive them with a synthetic tank."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import subprocess
from collections.abc import Awaitable, Callable
from dataclasses import replace
from pathlib import Path

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store, lens_opencv
from conqueror_ai.adapters.bridge_ws import BridgeLink
from conqueror_ai.adapters.mjpeg_source import MjpegSource
from conqueror_ai.domain import calib_math as cm
from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.types import Pose

Grab = Callable[[], Awaitable[NDArray[np.uint8]]]  # the newest picture, RGB
Ask = Callable[[str], Awaitable[str]]  # a question to the owner, the typed answer
CamSet = Callable[
    [float, float], Awaitable[None]
]  # pan, tilt from the centre; returns once the camera settled
Click = Callable[[NDArray[np.uint8], str], Awaitable[tuple[float, float]]]  # the owner clicks a point

QVGA = (320, 240)
MIN_LENS_PICTURES = 10
TILT_POINTS = [-15.0, -5.0, 5.0, 15.0, 30.0]
PAN_POINTS = [-30.0, -15.0, 0.0, 15.0, 30.0]
STUDY_TILTS = [0.0, -5.0, -10.0, -15.0, -20.0]
START = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


# ---------------------------------------------------------------- the steps
async def lens(
    grab: Grab, ask: Ask, out_dir: Path, show: Callable[[Path], None]
) -> tuple[NDArray[np.float64], NDArray[np.float64], float, int]:
    """The board on the screen, then pictures until the owner types q: K, dist, RMS (px), pictures used."""
    png = out_dir / "checkerboard.png"
    lens_opencv.checkerboard_png(png)
    show(png)
    square_mm = float(await ask("Measure one square of the board on the screen (mm): "))
    sets: list[NDArray[np.float64]] = []
    while True:
        a = (
            await ask(
                f"[{len(sets)} pictures] Enter = take one (tilt the tank a little each time), q = done: "
            )
        ).strip()
        if a == "q":
            if len(sets) >= MIN_LENS_PICTURES:
                break
            print(f"at least {MIN_LENS_PICTURES} pictures with the whole board, please")
            continue
        gray = np.asarray(cv2.cvtColor(await grab(), cv2.COLOR_RGB2GRAY), dtype=np.uint8)
        corners = lens_opencv.find_corners(gray)
        if corners is None:
            print("  the whole board was not found - is all of it in the picture?")
        else:
            sets.append(corners)
    k, dist, rms = lens_opencv.calibrate(sets, square_mm / 1000.0, QVGA)
    return k, dist, rms, len(sets)


async def mount_numbers(cal: Calibration, ask: Ask) -> Calibration:
    """The mount, measured with a ruler (cm), typed in."""

    async def cm_(q: str, now: float) -> float:
        a = (await ask(f"{q} [{now * 100:.1f} cm]: ")).strip()
        return float(a) / 100.0 if a else now

    return replace(
        cal,
        cam_height_m=await cm_("Lens height above the floor", cal.cam_height_m),
        cam_x_m=await cm_("Lens ahead of the tank's centre", cal.cam_x_m),
        cam_y_m=await cm_("Lens left of the tank's centre", cal.cam_y_m),
        track_front_m=await cm_("Tracks' front ahead of the tank's centre", cal.track_front_m),
    )


async def mount_tilt(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
    """The real tilt at the servo's centre, from the laser level's line on a wall at the lens height."""
    await cam_set(0.0, 0.0)
    while True:
        await ask("Laser level: a horizontal line on the wall ahead at exactly the lens height. Enter: ")
        row = cm.laser_row(await grab())
        if row is not None:
            tilt0 = cm.horizon_tilt_deg(cal, row)
            print(
                f"  horizon at row {row:.1f} -> the camera looks {abs(tilt0):.1f} deg "
                f"{'up' if tilt0 > 0 else 'down'}"
            )
            return replace(cal, tilt0_deg=tilt0)
        print("  no red line found - is it in the picture and bright enough?")


async def floor_marks(cal: Calibration, grab: Grab, click: Click, ask: Ask) -> list[float]:
    """Tape marks 50 and 100 cm ahead of the tracks' front: the model's error for each (m)."""
    await ask("Tape marks on the floor 50 and 100 cm straight ahead of the tracks' front. Enter: ")
    img = await grab()
    cam = CameraModel(cal).for_frame(START, 0.0, 0.0)
    errors: list[float] = []
    for d in (0.50, 1.00):
        u, v = await click(img, f"click the {d * 100:.0f} cm mark")
        e = cm.floor_mark_error_m(cam, u, v, d)
        errors.append(e if e is not None else float("inf"))
        print(f"  {d * 100:.0f} cm mark: off by {errors[-1] * 100:.1f} cm")
    return errors


async def servo_tilt(cal: Calibration, grab: Grab, cam_set: CamSet) -> Calibration:
    """Commanded tilt -> real tilt change, from the horizon line at each tilt (laser at lens height)."""
    real: list[float] = []
    for t in TILT_POINTS:
        await cam_set(0.0, t)
        row = cm.laser_row(await grab())
        if row is None:
            raise RuntimeError(f"no horizon line at tilt {t:+.0f} - keep the laser line at lens height")
        real.append(cm.horizon_tilt_deg(cal, row) - cal.tilt0_deg)
    await cam_set(0.0, 0.0)
    return replace(cal, tilt_map=cm.fit_linear(TILT_POINTS, real))


async def servo_pan(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
    """Commanded pan -> real pan, from the laser level's vertical line straight ahead, the camera level."""
    g, o = cal.tilt_map
    level = max(-20.0, min(60.0, (-cal.tilt0_deg - o) / g))
    await ask("Laser level: a vertical line straight ahead of the tank, on the wall. Enter: ")
    real: list[float] = []
    for p in PAN_POINTS:
        await cam_set(p, level)
        col = cm.laser_col(await grab())
        if col is None:
            raise RuntimeError(f"no vertical line at pan {p:+.0f} - is it straight ahead and bright enough?")
        real.append(cm.pan_from_col_deg(cal, col))
    await cam_set(0.0, 0.0)
    return replace(cal, pan_map=cm.fit_linear(PAN_POINTS, real))


def study_table(rows: list[cm.TiltRow]) -> str:
    lines = ["tilt   blind (from the tracks)   farthest floor   walls in view"]
    for r in rows:
        far = "-" if r.far_m == float("inf") else f"{r.far_m * 100:.0f} cm"
        lines.append(
            f"{r.tilt_deg:+5.0f}  {r.blind_m * 100:9.1f} cm {far:>23}   "
            f"{'yes' if r.horizon_in_view else 'no'}"
        )
    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------- the real tank
def _defaults() -> Calibration:
    """Before the lens step: Q0's measured mount (design-notes §109), a nominal QVGA lens."""
    k = np.array([[260.0, 0.0, 160.0], [0.0, 260.0, 120.0], [0.0, 0.0, 1.0]])
    return Calibration(
        k=k,
        dist=np.zeros(5),
        size=QVGA,
        cam_height_m=0.178,
        cam_x_m=0.05,
        cam_y_m=0.0,
        track_front_m=0.10,
        tilt0_deg=0.0,
    )


async def _ask(prompt: str) -> str:
    return await asyncio.to_thread(input, prompt)


def _show(path: Path) -> None:
    subprocess.run(["open", str(path)], check=False)  # macOS: Preview; the owner makes it full-screen


async def _click(img: NDArray[np.uint8], what: str) -> tuple[float, float]:
    """An OpenCV window; the event loop keeps running (the bridge drops a client silent for 3 s)."""
    got: list[tuple[float, float]] = []

    def on_mouse(event: int, x: int, y: int, _flags: int, _param: object) -> None:
        if event == cv2.EVENT_LBUTTONDOWN:
            got.append((float(x), float(y)))

    name = f"cq-calibrate: {what}"
    cv2.namedWindow(name, cv2.WINDOW_NORMAL)
    cv2.imshow(name, cv2.cvtColor(img, cv2.COLOR_RGB2BGR))
    cv2.setMouseCallback(name, on_mouse)
    while not got:
        cv2.waitKey(30)
        await asyncio.sleep(0)
    cv2.destroyWindow(name)
    cv2.waitKey(1)
    return got[0]


async def _run(step: str, host: str, out: Path) -> None:
    link = BridgeLink(host)
    cam = MjpegSource(host)
    stop = asyncio.Event()
    tasks = [asyncio.create_task(link.run(stop)), asyncio.create_task(cam.run(stop))]

    async def grab() -> NDArray[np.uint8]:
        f = await cam.next_frame()
        f = await cam.next_frame()  # a picture taken after the last move
        bgr = cv2.imdecode(np.frombuffer(f.jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
        return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).astype(np.uint8)

    async def cam_set(pan: float, tilt: float) -> None:
        while not link.connected:
            await asyncio.sleep(0.1)
        await link.cam_set(pan, tilt)
        await asyncio.sleep(1.0)  # the servos move at 120 deg/s; then a new picture

    try:
        cal = calibration_store.load(out, QVGA) if out.exists() else _defaults()
        if step == "lens":
            k, dist, rms, n = await lens(grab, _ask, out.parent, _show)
            print(f"lens: {n} pictures, error {rms:.2f} px ({'good' if rms < 0.5 else 'high - try again'})")
            cal = replace(cal, k=k, dist=dist)
        elif step == "mount":
            cal = await mount_numbers(cal, _ask)
            cal = await mount_tilt(cal, grab, cam_set, _ask)
            errors = await floor_marks(cal, grab, _click, _ask)
            print("mount: " + ("pass" if all(e <= 0.02 for e in errors) else "a mark is more than 2 cm off"))
        elif step == "servos":
            cal = await servo_tilt(cal, grab, cam_set)
            cal = await servo_pan(cal, grab, cam_set, _ask)
            print(
                f"servos: tilt {cal.tilt_map[0]:.3f} x + {cal.tilt_map[1]:+.2f}, "
                f"pan {cal.pan_map[0]:.3f} x + {cal.pan_map[1]:+.2f}"
            )
        elif step == "study":
            print(study_table(cm.tilt_study(cal, STUDY_TILTS)))
            return
        calibration_store.save(replace(cal, date=datetime.date.today().isoformat()), out)
        print(f"saved {out}")
    finally:
        stop.set()
        with contextlib.suppress(Exception):
            await link.close()
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def main() -> None:
    ap = argparse.ArgumentParser(description="Calibrate the tank's camera (AI engine LLD §8)")
    ap.add_argument("step", choices=["lens", "mount", "servos", "study"])
    ap.add_argument("--host", default="192.168.31.167")
    ap.add_argument("--out", type=Path, default=Path("config/calibration.yaml"))
    a = ap.parse_args()
    asyncio.run(_run(a.step, a.host, a.out))
