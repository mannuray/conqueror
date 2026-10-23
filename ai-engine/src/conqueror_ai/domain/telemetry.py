"""The bridge's `td` message -> TelemSample (bridge LLD R5 + Plan Q1 `bus_us`). Pure."""

from __future__ import annotations

from typing import Any

from conqueror_ai.domain.types import TelemSample


def _num(m: dict[str, Any], key: str) -> float:
    v = m[key]
    if isinstance(v, bool) or not isinstance(v, int | float):
        raise ValueError(f"{key} is not a number")
    return float(v)


def _pair(m: dict[str, Any], key: str) -> tuple[int, int]:
    v = m[key]
    if not isinstance(v, list) or len(v) != 2:  # pyright: ignore[reportUnknownArgumentType]
        raise ValueError(f"{key} is not a pair")
    a, b = v[0], v[1]  # pyright: ignore[reportUnknownVariableType]
    if not isinstance(a, int) or not isinstance(b, int):
        raise ValueError(f"{key} is not a pair of integers")
    return a, b


def parse_td(m: dict[str, Any]) -> TelemSample | None:
    """A `td` message, or None when it is something else or broken (a bridge without `bus_us` included:
    the engine needs the stamp)."""
    if m.get("t") != "td":
        return None
    try:
        enc_l, enc_r = _pair(m, "enc")
        cmd_l, cmd_r = _pair(m, "cmd")
        pwm_l, pwm_r = _pair(m, "pwm")
        dist = m.get("dist_mm")
        return TelemSample(
            bus_us=int(_num(m, "bus_us")),
            uno_ms=int(_num(m, "uno_ms")),
            enc_l=enc_l,
            enc_r=enc_r,
            yaw_deg=_num(m, "yaw"),
            rate_dps=_num(m, "rate"),
            dist_mm=int(dist) if isinstance(dist, int | float) and not isinstance(dist, bool) else None,
            pan_deg=_num(m, "pan"),
            tilt_deg=_num(m, "tilt"),
            cmd_l=cmd_l,
            cmd_r=cmd_r,
            flags=int(_num(m, "flags")),
            pwm_l=pwm_l,
            pwm_r=pwm_r,
        )
    except (KeyError, ValueError):
        return None
