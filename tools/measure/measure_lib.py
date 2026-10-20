"""Phase 0 measurements - the pure parts (tested in tests/test_measure_lib.py, no tank needed)."""

from __future__ import annotations

import statistics
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass


class MjpegParser:
    """Splits a multipart MJPEG byte stream into JPEG pictures (the bridge's `:81/stream`)."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> Iterator[bytes]:
        """Adds bytes; yields every complete JPEG (from FFD8 to FFD9) found so far."""
        self._buf += chunk
        while True:
            start = self._buf.find(b"\xff\xd8")
            if start < 0:
                del self._buf[:-1]  # keep a possible first half of a marker
                return
            end = self._buf.find(b"\xff\xd9", start + 2)
            if end < 0:
                del self._buf[:start]
                return
            yield bytes(self._buf[start : end + 2])
            del self._buf[: end + 2]


def rising_edges(times: Sequence[float], values: Sequence[float], threshold: float) -> list[float]:
    """Times where the value goes from below `threshold` to at or above it (screen turned white)."""
    out: list[float] = []
    for i in range(1, len(values)):
        if values[i - 1] < threshold <= values[i]:
            out.append(times[i])
    return out


def match_delays(flashes: Sequence[float], edges: Sequence[float], max_s: float = 1.0) -> list[float]:
    """For each flash, the delay to the first edge after it (within `max_s`); unmatched flashes skipped."""
    out: list[float] = []
    j = 0
    for f in flashes:
        while j < len(edges) and edges[j] < f:
            j += 1
        if j < len(edges) and edges[j] - f <= max_s:
            out.append(edges[j] - f)
            j += 1
    return out


@dataclass(frozen=True)
class DepthFit:
    a: float
    c: float
    rows: list[tuple[float, float, float]]  # (tape m, model m, error %)


def fit_inverse_depth(tape_m: Sequence[float], rel: Sequence[float], inverse: bool = True) -> DepthFit:
    """Least-squares fit 1/d = a*r + c (Depth Anything gives relative inverse depth r), or d = a*r + c with
    inverse=False (if a model's output is plain depth), with the error of the fitted distance per mark."""
    if len(tape_m) != len(rel) or len(tape_m) < 2:
        raise ValueError("need at least two (tape, relative) pairs")
    ys = [1.0 / d if inverse else d for d in tape_m]
    mx, my = statistics.fmean(rel), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in rel)
    if sxx == 0:
        raise ValueError("all relative values are equal")
    a = sum((x - mx) * (y - my) for x, y in zip(rel, ys, strict=True)) / sxx
    c = my - a * mx
    rows: list[tuple[float, float, float]] = []
    for d, r in zip(tape_m, rel, strict=True):
        y = a * r + c
        model = (1.0 / y if y > 0 else float("inf")) if inverse else y
        rows.append((d, model, (model - d) / d * 100.0))
    return DepthFit(a, c, rows)


@dataclass(frozen=True)
class SonarStats:
    samples: int
    no_echo_pct: float
    median_mm: float | None
    spread_mm: float | None  # 10-90 % range of the valid readings


def sonar_stats(dist_mm: Iterable[int | None], max_mm: int = 4000) -> SonarStats:
    """`None`, a fault or anything at / over `max_mm` counts as "no echo"."""
    all_ = list(dist_mm)
    valid = sorted(d for d in all_ if d is not None and 0 < d < max_mm)
    if not all_:
        return SonarStats(0, 0.0, None, None)
    no_echo = 100.0 * (len(all_) - len(valid)) / len(all_)
    if not valid:
        return SonarStats(len(all_), no_echo, None, None)
    p10, p90 = valid[int(0.1 * (len(valid) - 1))], valid[int(0.9 * (len(valid) - 1))]
    return SonarStats(len(all_), no_echo, float(statistics.median(valid)), float(p90 - p10))


@dataclass(frozen=True)
class GyroStats:
    minutes: float
    drift_deg_per_min: float  # heading change per minute while standing still
    rate_mean_dps: float  # the gyro's reading at rest (its bias)
    rate_std_dps: float


def unwrap_deg(values: Sequence[float]) -> list[float]:
    """Heading without the jumps at +-180."""
    out: list[float] = []
    off = 0.0
    for i, v in enumerate(values):
        if i and v - values[i - 1] > 180:
            off -= 360
        elif i and v - values[i - 1] < -180:
            off += 360
        out.append(v + off)
    return out


def gyro_stats(t_s: Sequence[float], yaw_deg: Sequence[float], rate_dps: Sequence[float]) -> GyroStats:
    if len(t_s) < 2 or t_s[-1] <= t_s[0]:
        raise ValueError("need a recording longer than zero seconds")
    yaw = unwrap_deg(yaw_deg)
    minutes = (t_s[-1] - t_s[0]) / 60.0
    return GyroStats(
        minutes,
        (yaw[-1] - yaw[0]) / minutes,
        statistics.fmean(rate_dps),
        statistics.pstdev(rate_dps),
    )
