"""The live view (AI engine LLD §4 LiveView): the camera picture and the telemetry in the Rerun viewer, on the
bridge's clock. Also writes a `.rrd` when given a path (open it later with `rerun file.rrd`)."""

from __future__ import annotations

from pathlib import Path

import rerun as rr

from conqueror_ai.domain.types import Frame, TelemSample


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
