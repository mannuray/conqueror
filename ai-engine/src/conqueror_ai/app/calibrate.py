"""`cq-calibrate` - the calibration tool (AI engine LLD §8, design-notes §123). Steps, in order:

  lens    the checkerboard on the laptop screen, ~20 pictures -> focal length, centre, distortion
  mount   the measured mount, the laser level's horizon line -> the real tilt, then two tape marks to check
  servos  commanded -> real tilt and pan gains: a laser cross line of any colour, roughly at the lens height
          (design-notes §130); tilt0 and the pan centre are kept
  study   the blind zone and the view at a few tilts, for the owner's driving-tilt decision

Each step saves config/calibration.yaml. The tool's only command to the tank is cam_set (bridge LLD R22): it
never drives. The steps take their inputs as functions, so tests drive them with a synthetic tank."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import datetime
import subprocess
import threading
from collections.abc import Awaitable, Callable
from dataclasses import replace
from pathlib import Path

import cv2
import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store, lens_opencv
from conqueror_ai.adapters.bridge_ws import BridgeLink, CommandRefused
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
MIN_SERVO_POINTS = 3  # a missed point is skipped; at least this many make the fit (final review I2)
CONNECT_S = 10.0
STUDY_TILTS = [0.0, -5.0, -10.0, -15.0, -20.0]
START = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


class Refused(Exception):
    """A step cannot go on; its message says why in plain words."""


def qvga(img: NDArray[np.uint8]) -> NDArray[np.uint8]:
    """The picture, if it is 320 x 240 - the size this calibration is for (final review I4)."""
    if img.shape[:2] != (QVGA[1], QVGA[0]):
        raise ValueError(
            f"pictures must be 320 x 240 (QVGA), got {img.shape[1]} x {img.shape[0]} - "
            "set the camera to QVGA in Settings, then run this step again"
        )
    return img


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
        gray = np.asarray(cv2.cvtColor(qvga(await grab()), cv2.COLOR_RGB2GRAY), dtype=np.uint8)
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
    """The real tilt at the servo's centre, from the laser level's line on a wall at the lens height.
    Stored so that tilt0 + the servo map's offset at command 0 is the measured tilt - never counted twice
    (final review I3)."""
    while True:
        await ask("Laser level: a horizontal line on the wall ahead at exactly the lens height. Enter: ")
        await cam_set(0.0, 0.0)  # again just before the picture: a released servo may have sagged
        row = cm.laser_row(qvga(await grab()))
        if row is not None:
            tilt0 = cm.horizon_tilt_deg(cal, row) - cal.tilt_map[1]
            print(
                f"  horizon at row {row:.1f} -> the camera looks {abs(tilt0):.1f} deg "
                f"{'up' if tilt0 > 0 else 'down'}"
            )
            return replace(cal, tilt0_deg=tilt0)
        print("  no laser line found - is it in the picture and bright enough?")


async def floor_marks(cal: Calibration, grab: Grab, click: Click, ask: Ask, cam_set: CamSet) -> list[float]:
    """Tape marks 50 and 100 cm ahead of the tracks' front: the model's error for each (m)."""
    await ask("Tape marks on the floor 50 and 100 cm straight ahead of the tracks' front. Enter: ")
    await cam_set(0.0, 0.0)
    img = qvga(await grab())
    cam = CameraModel(cal).for_frame(START, 0.0, 0.0)
    errors: list[float] = []
    for d in (0.50, 1.00):
        u, v = await click(img, f"click the {d * 100:.0f} cm mark")
        e = cm.floor_mark_error_m(cam, u, v, d)
        errors.append(e if e is not None else float("inf"))
        print(f"  {d * 100:.0f} cm mark: off by {errors[-1] * 100:.1f} cm")
    return errors


MAX_FIT_ERROR_DEG = (
    1.5  # a picture this far off its way's straight line: something else was taken for the line
)


async def _sweep(
    points: list[float],
    limits: tuple[float, float],
    measure: Callable[[float], Awaitable[float | None]],
    what: str,
) -> tuple[float, float]:
    """Both ways across `points` (design-notes §130): down, every point reached from above (a move to just
    above the top first), then up, every point reached from below (a dip just below the lowest first). Where
    the servo's limit leaves no room, the point next to it is left out of that way. Each way gets its own
    straight-line fit, so the slack cancels out of the gain. Returns (gain, slack): the mean gain, and half
    the gap between the two ways at the middle of the points (deg; + = lands higher / further right arriving
    from above / the right)."""
    lo_lim, hi_lim = limits
    top, bottom = max(points), min(points)
    down = sorted(points, reverse=True)
    up = sorted(points)
    fits: list[tuple[float, float]] = []
    for way, pre, ok in ((down, top + 2.0, top + 2.0 <= hi_lim), (up, bottom - 2.0, bottom - 2.0 >= lo_lim)):
        if ok:
            await measure(pre)  # only to reach the first point from the right side (its answer is not used)
        else:
            way = way[1:]  # no room past the limit: that point would be reached from the wrong side
        done: list[float] = []
        real: list[float] = []
        for x in way:
            r = await measure(x)
            if r is None:
                print(f"  no line at {what} {x:+.1f} - skipped")
                continue
            done.append(x)
            real.append(r)
        if len(done) < MIN_SERVO_POINTS:
            raise Refused(f"too few {what}s saw the line - is it in the picture and bright enough?")
        g, o = cm.fit_linear(done, real)
        worst = max(abs(y - (g * x + o)) for x, y in zip(done, real, strict=True))
        if worst > MAX_FIT_ERROR_DEG:
            raise Refused(
                f"one {what} picture's line is {worst:.1f} deg off the others - is something else bright or "
                "red in the picture?"
            )
        fits.append((g, o))
    (g_down, o_down), (g_up, o_up) = fits
    mid = float(np.mean(points))
    return (g_down + g_up) / 2.0, ((g_down * mid + o_down) - (g_up * mid + o_up)) / 2.0


def _slack_text(slack: float, higher: str, side: str) -> str:
    return f"lands {abs(slack):.1f} deg {higher if slack >= 0 else 'the other way'} arriving from {side}"


async def servo_tilt(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
    """Commanded tilt -> real tilt: the gain only (design-notes §130). The line may be a few cm off the lens
    height, because only the change between pictures counts. tilt0 and the map's offset are kept as they are,
    so nothing is counted twice (final review I3). The camera is back at the centre afterwards, also on a
    refusal."""
    await ask(
        "Laser level: the horizontal line on the wall ahead, roughly at the lens height (+-5 cm). Enter: "
    )
    try:
        await cam_set(0.0, 0.0)
        row0 = cm.laser_row(qvga(await grab()))
        if row0 is None:
            raise Refused("no horizontal line at the centre - is it in the picture and bright enough?")
        points = cm.tilt_points(cal, cm.horizon_tilt_deg(cal, row0))

        async def measure(t: float) -> float | None:
            await cam_set(0.0, t)
            row = cm.laser_row(qvga(await grab()))
            return None if row is None else cm.horizon_tilt_deg(cal, row)

        gain, slack = await _sweep(points, (-20.0, 60.0), measure, "tilt")
    finally:
        await cam_set(0.0, 0.0)
    print(f"  tilt: {gain:.3f} real deg per commanded deg; slack: {_slack_text(slack, 'higher', 'above')}")
    return replace(cal, tilt_map=(gain, cal.tilt_map[1]))


async def servo_pan(cal: Calibration, grab: Grab, cam_set: CamSet, ask: Ask) -> Calibration:
    """Commanded pan -> real pan: the gain only (design-notes §130), from the laser level's vertical line
    anywhere in the picture, the camera level. The map's offset (the Uno's camera centre: 0) is kept. The
    camera is back at the centre afterwards, also on a refusal."""
    g, o = cal.tilt_map
    level = max(-20.0, min(60.0, (-cal.tilt0_deg - o) / g))
    await ask("Laser level: the vertical line on the wall ahead, roughly straight ahead. Enter: ")
    try:
        await cam_set(0.0, level)
        col0 = cm.laser_col(qvga(await grab()))
        if col0 is None:
            raise Refused("no vertical line at the centre - is it in the picture and bright enough?")
        points = cm.pan_points(cal, cm.pan_from_col_deg(cal, col0))

        async def measure(p: float) -> float | None:
            await cam_set(p, level)
            col = cm.laser_col(qvga(await grab()))
            return None if col is None else cm.pan_from_col_deg(cal, col)

        gain, slack = await _sweep(points, (-90.0, 90.0), measure, "pan")
    finally:
        await cam_set(0.0, 0.0)
    slack_txt = _slack_text(slack, "further right", "the right")
    print(f"  pan: {gain:.3f} real deg per commanded deg; slack: {slack_txt}")
    return replace(cal, pan_map=(gain, cal.pan_map[1]))


def study_table(rows: list[cm.TiltRow]) -> str:
    lines = ["tilt   blind ahead   blind at the edge   farthest floor   walls in view"]
    for r in rows:
        far = "-" if r.far_m == float("inf") else f"{r.far_m * 100:.0f} cm"
        edge = "-" if r.blind_edge_m == float("inf") else f"{r.blind_edge_m * 100:.1f} cm"
        lines.append(
            f"{r.tilt_deg:+5.0f} {r.blind_m * 100:9.1f} cm {edge:>15} {far:>17}   "
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
    """input() in a daemon thread: Ctrl-C ends the tool without waiting for Enter."""
    loop = asyncio.get_running_loop()
    fut: asyncio.Future[str] = loop.create_future()

    def read() -> None:
        try:
            line = input(prompt)
        except EOFError:
            line = "q"
        loop.call_soon_threadsafe(lambda: fut.done() or fut.set_result(line))

    threading.Thread(target=read, daemon=True).start()
    return await fut


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
        if cv2.getWindowProperty(name, cv2.WND_PROP_VISIBLE) < 1:
            raise Refused("the picture window was closed before a click")
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
        for _ in range(int(CONNECT_S / 0.1)):
            if link.connected:
                break
            await asyncio.sleep(0.1)
        else:
            raise Refused(f"no connection to the tank at {host} after {CONNECT_S:.0f} s - is it on?")
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
            errors = await floor_marks(cal, grab, _click, _ask, cam_set)
            if all(e <= 0.02 for e in errors):
                print("mount: pass")
            else:
                print("mount: a mark is more than 2 cm off - check the measured height and positions")
                if (await _ask("Save anyway? (y/N): ")).strip().lower() != "y":
                    print("not saved")
                    return
        elif step == "servos":
            cal = await servo_tilt(cal, grab, cam_set, _ask)
            cal = await servo_pan(cal, grab, cam_set, _ask)
            print(f"servos: tilt {cal.tilt_map[0]:.3f} x, pan {cal.pan_map[0]:.3f} x (tilt0, centre kept)")
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
    try:
        asyncio.run(_run(a.step, a.host, a.out))
    except (Refused, ValueError, ConnectionError, TimeoutError) as e:
        raise SystemExit(f"cq-calibrate {a.step}: {e}") from None
    except CommandRefused as e:
        hint = " - release the phone's drive stick first" if e.reason == "NOT_OWNER" else ""
        raise SystemExit(f"cq-calibrate {a.step}: the tank refused the camera move ({e}){hint}") from None
    except KeyboardInterrupt:
        raise SystemExit(f"cq-calibrate {a.step}: stopped") from None
