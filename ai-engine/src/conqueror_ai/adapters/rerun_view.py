"""The live view (AI engine LLD §4 LiveView): the camera picture and the telemetry in the Rerun viewer, on the
bridge's clock. Also writes a `.rrd` when given a path (open it later with `rerun file.rrd`)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import rerun as rr

from conqueror_ai.domain.camera import FloatArray
from conqueror_ai.domain.depth import AngleFix, Marks
from conqueror_ai.domain.types import Detection, Frame, ObjectEstimate, Pose, TelemSample


class RerunView:
    def __init__(self, spawn: bool = True, save: Path | None = None) -> None:
        rr.init("conqueror-ai", spawn=spawn)
        if save is not None:
            rr.save(str(save))

    def telemetry(self, s: TelemSample) -> None:
        rr.set_time("bridge", duration=s.bus_us / 1e6)
        rr.log("tank/heading_deg", rr.Scalars(s.yaw_deg))
        rr.log("tank/turn_rate_dps", rr.Scalars(s.rate_dps))
        rr.log("tank/encoders", rr.Scalars([float(s.enc_l), float(s.enc_r)]))
        if s.dist_mm is not None:
            rr.log("tank/ultrasonic_cm", rr.Scalars(s.dist_mm / 10.0))

    def frame(self, f: Frame, age_s: float | None) -> None:
        rr.set_time("bridge", duration=f.cap_us / 1e6)
        rr.log("camera/image", rr.EncodedImage(contents=f.jpeg, media_type="image/jpeg"))
        if age_s is not None:
            rr.log("camera/age_on_arrival_ms", rr.Scalars(age_s * 1000.0))

    def detections(self, f: Frame, dets: list[Detection]) -> None:
        """The detector's boxes over the picture (AI engine LLD §15.6)."""
        rr.set_time("bridge", duration=f.cap_us / 1e6)
        rr.log(
            "camera/image/boxes",
            rr.Boxes2D(
                array=[list(d.box) for d in dets],
                array_format=rr.Box2DFormat.XYXY,
                labels=[f"{d.cls} {d.score:.2f}" for d in dets],
            ),
        )

    def plan(self, t_us: int, pose: Pose | None, objs: list[ObjectEstimate], skipped: str | None) -> None:
        """The room from above: the tank and the things (seen: solid, remembered: faded); y left is up."""
        rr.set_time("bridge", duration=t_us / 1e6)
        if pose is not None:
            rr.log("plan/tank", rr.Points2D([[pose.x_m, pose.y_m]], radii=0.08, colors=[[80, 160, 255]]))
        rr.log(
            "plan/things",
            rr.Points2D(
                [[o.x_m, o.y_m] for o in objs],
                radii=[max(0.05, o.sigma_m) for o in objs],
                colors=[[255, 200, 0] if o.state == "seen" else [120, 100, 40] for o in objs],
                labels=[f"#{o.oid} {o.cls}" for o in objs],
            ),
        )
        rr.log("plan/skipped", rr.TextLog(skipped or "used"))

    # ---- seeing (AI engine LLD §18)
    def depth(self, f: Frame, range_m: FloatArray) -> None:
        """The depth picture in metres along each ray (0 = unknown)."""
        rr.set_time("bridge", duration=f.cap_us / 1e6)
        rr.log(
            "camera/depth",
            rr.DepthImage(np.where(np.isfinite(range_m), range_m, 0.0).astype(np.float32), meter=1.0),
        )

    def marks(self, t_us: int, marks: Marks) -> None:
        """Every point by height, from above: floor grey, low red, high orange, above light blue."""
        rr.set_time("bridge", duration=t_us / 1e6)
        for name, xy, colour in (
            ("floor", marks.floor_xy, [150, 150, 150]),
            ("low", marks.low_xy, [230, 40, 40]),
            ("high", marks.high_xy, [255, 150, 0]),
            ("above", marks.above_xy, [140, 200, 255]),
        ):
            rr.log(f"plan/{name}", rr.Points2D(xy, radii=0.02, colors=[colour]))

    def angle(self, t_us: int, fix: AngleFix) -> None:
        """The tilt fix (degrees) and whether it is trusted."""
        rr.set_time("bridge", duration=t_us / 1e6)
        rr.log("camera/tilt_fix_deg", rr.Scalars(fix.tilt_fix_deg))
        rr.log("camera/tilt_trusted", rr.Scalars(1.0 if fix.trusted else 0.0))
