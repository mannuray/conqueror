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
    pwm_l: int = 0  # the motor output, -255..255: its sign is the track's direction (LLD §14)
    pwm_r: int = 0


@dataclass(frozen=True)
class Frame:
    """One camera picture with its capture time (VSYNC) on the bridge's clock."""

    seq: int
    cap_us: int
    recv_s: float  # laptop monotonic time when the whole picture had arrived
    jpeg: bytes


@dataclass(frozen=True)
class Pose:
    """The tank in the room (LLD §3): start = 0, 0, 0; x forward at the start, y left, th counter-clockwise
    (ROS REP 103, as the bridge's arcs)."""

    t_us: int
    x_m: float
    y_m: float
    th_rad: float
    v_mps: float
    w_rps: float


@dataclass(frozen=True)
class Detection:
    """One box from the detector (LLD §15.1), in the picture's pixels."""

    cls: str
    score: float
    box: tuple[float, float, float, float]  # x1, y1, x2, y2


@dataclass(frozen=True)
class ObjectEstimate:
    """One thing on the list (LLD §15.4), in the room frame."""

    oid: int
    cls: str
    x_m: float
    y_m: float
    vx: float
    vy: float
    sigma_m: float  # the position's standard deviation (the larger axis)
    first_seen_us: int
    last_seen_us: int
    state: str  # "seen" or "remembered"
