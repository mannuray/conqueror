"""The bridge's clock (ESP32 timer, µs) against the laptop's monotonic clock (AI engine LLD §5.1). Pure.

Only for ages and latency: pairing a picture with telemetry uses the bridge's clock on both sides.
"""

from __future__ import annotations

import statistics


class BridgeClock:
    """Each ping: the laptop's send time t0 and receive time t1 (seconds, monotonic) and the pong's
    `bridge_us`. The offset comes from the `keep` lowest-round-trip samples of the last `window_s`."""

    def __init__(self, keep: int = 8, window_s: float = 60.0) -> None:
        self._keep = keep
        self._window = window_s
        self._samples: list[tuple[float, float, float]] = []  # (t1, round trip, offset)

    def add(self, t0_s: float, t1_s: float, bridge_us: int) -> None:
        if t1_s < t0_s or bridge_us <= 0:
            return  # a pong from a bridge without the clock (bridge_us 0) is ignored
        rtt = t1_s - t0_s
        self._samples.append((t1_s, rtt, bridge_us / 1e6 - (t0_s + t1_s) / 2))
        self._samples = [s for s in self._samples if s[0] >= t1_s - self._window]

    def ready(self) -> bool:
        return bool(self._samples)

    def offset_s(self) -> float:
        """bridge seconds - laptop seconds."""
        if not self._samples:
            raise ValueError("no pong with bridge_us yet")
        best = sorted(self._samples, key=lambda s: s[1])[: self._keep]
        return statistics.median(s[2] for s in best)

    def to_local(self, bridge_us: int) -> float:
        """A bridge time as laptop monotonic seconds."""
        return bridge_us / 1e6 - self.offset_s()

    def age_s(self, bridge_us: int, now_s: float) -> float:
        return now_s - self.to_local(bridge_us)
