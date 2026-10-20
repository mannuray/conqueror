"""Data the engine passes around (AI engine LLD §3). Pure: no I/O."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class TelemSample:
    """One `td` message (50 Hz): what the Uno measured, stamped by the bridge on arrival (`bus_us`)."""

    bus_us: int  # ESP32 timer (µs since boot) when the Uno's frame arrived - the camera frames' clock too
    uno_ms: int
    enc_l: int
    enc_r: int
    yaw_deg: float
    rate_dps: float
    dist_mm: int | None  # None: no echo or sensor fault
    pan_deg: float
    tilt_deg: float
    cmd_l: int
    cmd_r: int
    flags: int


@dataclass(frozen=True)
class Frame:
    """One camera picture with its capture time (VSYNC) on the bridge's clock."""

    seq: int
    cap_us: int
    recv_s: float  # laptop monotonic time when the whole picture had arrived
    jpeg: bytes
