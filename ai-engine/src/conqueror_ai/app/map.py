"""`cq-map`: the room map (AI engine LLD §18.5, spec 9 §3) - the depth picture's height marks and the distance
sensor's echoes on one 8 x 8 m map fixed where the tank started. It only watches; it never drives: drive with
the phone and watch the map fill (red low, orange high, grey seen, black unseen, the tank white).

    uv run cq-map recordings/20261028-101500      # replay: every picture, in order; prints the map's numbers
    uv run cq-map --live [--record DIR]           # the tank: the live view of the map

Without the depth model (--no-depth, or no file) only the sensor's echoes mark the map.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import time
from collections.abc import Callable
from pathlib import Path

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.map_config import load_map_config
from conqueror_ai.adapters.session import replay
from conqueror_ai.app.objects import (
    CALIBRATION,
    DEFAULT_HOST,
    DEPTH_MODEL,
    MAP_CONFIG,
    VGA,
    DepthFn,
    decode,
    depth_due,
    load_depth,
    see,
)
from conqueror_ai.domain.camera import Calibration, CameraModel
from conqueror_ai.domain.depth import MapConfig, Seeing, Seen
from conqueror_ai.domain.objects import ObjectPipeline
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.sonar_model import SonarModel
from conqueror_ai.domain.telemetry import parse_td
from conqueror_ai.domain.types import Frame, TelemSample


class MapRun:
    """Telemetry and pictures in arrival order -> the room map (the depth model handed in)."""

    def __init__(self, cal: Calibration, cfg: MapConfig, depth: DepthFn | None) -> None:
        self.cfg = cfg
        self.depth = depth
        self.p = ObjectPipeline(cal, frozenset())  # the pose and each picture's camera; nothing is detected
        self.room = RoomMap(cfg.map)
        self.sonar = SonarModel(max_m=cfg.ranges.sonar_max_m)
        self.seeing = Seeing(cfg)
        self.angle_trusted = True
        self.used = 0
        self.depth_pictures = 0
        self.marked = 0
        self.echoes = 0

    def on_calib(self, counts_per_m: int) -> None:
        self.p.on_calib(counts_per_m)

    def on_telem(self, s: TelemSample) -> None:
        self.p.on_telem(s)
        pose = self.p.pose()
        dist = None if s.dist_mm is None or s.dist_mm <= 0 else s.dist_mm / 1000.0
        if pose is not None and self.sonar.update(self.room, pose, dist, s.bus_us):
            self.echoes += 1

    def on_text(self, text: str) -> None:
        """One recorded message: the tank's calibration or a telemetry sample."""
        m = json.loads(text)
        if m.get("t") == "calib" and isinstance(m.get("counts_per_m"), int):
            self.on_calib(int(m["counts_per_m"]))
            return
        s = parse_td(m)
        if s is not None:
            self.on_telem(s)

    def gate(self, f: Frame, rgb: NDArray[np.uint8]) -> CameraModel | None:
        """The picture's camera when the map uses it (not turning, the camera still), else None."""
        cam = self.p.on_frame(f, (rgb.shape[1], rgb.shape[0]), [], self.p.camera_at(f.cap_us))
        if isinstance(cam, str):
            return None
        self.used += 1
        return cam

    def depth_due(self) -> bool:
        return self.depth is not None and depth_due(self.used, self.cfg.depth.every_n)

    def apply(self, seen: Seen, t_us: int) -> None:
        """A seen picture onto the map."""
        self.depth_pictures += 1
        self.angle_trusted = seen.fix.trusted
        if seen.marks is not None:
            self.room.update_picture(seen.marks, t_us)
            self.marked += 1

    def on_picture(self, f: Frame, rgb: NDArray[np.uint8]) -> Seen | None:
        """One picture, all here (the replay)."""
        cam = self.gate(f, rgb)
        if cam is None or self.depth is None or not self.depth_due():
            return None
        seen = see(self.seeing, self.p, cam, self.depth(rgb), f.cap_us)
        self.apply(seen, f.cap_us)
        return seen

    def snapshot(self, t_us: int, stale: bool) -> MapSnapshot:
        return self.room.snapshot(t_us, self.p.pose(), stale, self.angle_trusted)


Show = Callable[[MapRun, Frame], None]


def run_replay(
    folder: Path, cal: Calibration, cfg: MapConfig, depth: DepthFn | None, show: Show | None = None
) -> MapRun:
    """Every recorded message and picture, in order (the same map every time)."""
    run = MapRun(cal, cfg, depth)
    for e in replay(folder):
        if e.text is not None:
            run.on_text(e.text)
        elif e.frame is not None:
            rgb = decode(e.frame)
            if rgb is None:
                continue
            run.on_picture(e.frame, rgb)
            if show is not None:
                show(run, e.frame)
    return run


def report(run: MapRun) -> str:
    room = run.room
    seen_m2 = float(room.seen.sum()) * room.cell_m**2
    b = run.cfg.map.blocked
    low = int(((room.low > b) & ~(room.high > b)).sum())
    high = int((room.high > b).sum())
    return "\n".join(
        [
            f"pictures used {run.used}, with depth {run.depth_pictures}, with marks {run.marked}; "
            f"echoes used {run.echoes}",
            f"map: seen {seen_m2:.1f} m², blocked low {low} cells, high {high} cells",
        ]
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", type=Path, nargs="?")
    ap.add_argument("--live", action="store_true")
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--record", type=Path, help="with --live: record the session to this folder")
    ap.add_argument("--calibration", type=Path, default=CALIBRATION)
    ap.add_argument("--depth-model", type=Path, default=DEPTH_MODEL)
    ap.add_argument("--no-depth", action="store_true", help="leave the depth model out (echoes only)")
    ap.add_argument("--no-view", action="store_true", help="no live view (the numbers only)")
    a = ap.parse_args()
    if a.live == (a.folder is not None):
        raise SystemExit("give a recording folder, or --live")
    if a.live:
        raise SystemExit("--live comes in Task 5")
    try:
        cal = calibration_store.load(a.calibration, VGA)
        cfg = load_map_config(MAP_CONFIG)
    except (FileNotFoundError, ValueError, KeyError) as err:
        raise SystemExit(str(err)) from None
    depth = None if a.no_depth else load_depth(a.depth_model)
    show: Show | None = None
    if not a.no_view:
        from conqueror_ai.adapters.rerun_view import RerunView

        view = RerunView(spawn=True)

        def to_view(run: MapRun, f: Frame) -> None:
            view.frame(f, None)
            view.room_map(f.cap_us, run.snapshot(f.cap_us, False))

        show = to_view

    t = time.monotonic()
    with contextlib.suppress(KeyboardInterrupt):
        run = run_replay(a.folder, cal, cfg, depth, show)
        print(report(run))
        print(f"laptop: {time.monotonic() - t:.1f} s")


if __name__ == "__main__":
    main()
