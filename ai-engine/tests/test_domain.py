from __future__ import annotations

from typing import Any

import pytest

from conqueror_ai.domain.clock import BridgeClock
from conqueror_ai.domain.mjpeg import MjpegParser, Part
from conqueror_ai.domain.telemetry import parse_td

TD: dict[str, Any] = {
    "t": "td",
    "seq": 3,
    "uno_ms": 1234,
    "bus_us": 4294967296123,
    "enc": [100, -5],
    "pwm": [130, -130],
    "cmd": [25, -25],
    "act": [None, 12],
    "yaw": 90.5,
    "rate": -12.5,
    "pitch": -1.25,
    "roll": 3.0,
    "dist_mm": 424,
    "closing": -7,
    "pan": -15.5,
    "tilt": 20.0,
    "flags": 5,
    "prim": 1,
    "pct": 40,
}


def test_td_becomes_a_sample() -> None:
    s = parse_td(TD)
    assert s is not None
    assert (s.bus_us, s.uno_ms, s.enc_l, s.enc_r) == (4294967296123, 1234, 100, -5)
    assert (s.yaw_deg, s.rate_dps, s.dist_mm, s.pan_deg, s.tilt_deg) == (90.5, -12.5, 424, -15.5, 20.0)
    assert (s.cmd_l, s.cmd_r, s.flags) == (25, -25, 5)


def test_no_echo_is_none() -> None:
    s = parse_td({**TD, "dist_mm": None, "dist_fault": True})
    assert s is not None and s.dist_mm is None


def test_other_or_broken_messages_are_skipped() -> None:
    assert parse_td({"t": "tf"}) is None
    assert parse_td({k: v for k, v in TD.items() if k != "bus_us"}) is None  # an older bridge: no stamp
    assert parse_td({**TD, "enc": [1]}) is None
    assert parse_td({**TD, "yaw": "x"}) is None


def test_clock_offset_from_the_fastest_pings() -> None:
    c = BridgeClock(keep=2)
    assert not c.ready()
    # bridge = laptop + 100 s; the slow ping (0.5 s) is misleading, the two fast ones are right
    c.add(10.0, 10.010, int((10.005 + 100) * 1e6))
    c.add(11.0, 11.5, int((11.0 + 100) * 1e6))
    c.add(12.0, 12.002, int((12.001 + 100) * 1e6))
    assert c.offset_s() == pytest.approx(100.0, abs=1e-3)
    assert c.to_local(int(105e6)) == pytest.approx(5.0, abs=1e-3)
    assert c.age_s(int(105e6), 5.25) == pytest.approx(0.25, abs=1e-3)


def test_clock_ignores_pongs_without_bridge_time_and_old_samples() -> None:
    c = BridgeClock(window_s=60)
    c.add(1.0, 1.01, 0)
    assert not c.ready()
    c.add(1.0, 1.01, int(50e6))
    c.add(100.0, 100.01, int(250e6))  # 60+ s later: the first sample leaves the window
    assert c.offset_s() == pytest.approx(150.0 - 0.005, abs=1e-3)
    with pytest.raises(ValueError):
        BridgeClock().offset_s()


def part(jpeg: bytes, us: int | None, seq: int) -> bytes:
    stamp = b"" if us is None else b"X-Frame-Us: %d\r\nX-Frame-Seq: %d\r\n" % (us, seq)
    return (
        (b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n" % len(jpeg))
        + stamp
        + b"\r\n"
        + jpeg
    )


def test_mjpeg_parts_with_their_capture_time_across_chunks() -> None:
    a, b = b"\xff\xd8" + b"A" * 50 + b"\xff\xd9", b"\xff\xd8BB\xff\xd9"
    stream = part(a, 123456789012, 1) + part(b, 123456822345, 2)
    p = MjpegParser()
    got: list[Part] = []
    for i in range(0, len(stream), 9):
        got += list(p.feed(stream[i : i + 9]))
    assert got == [Part(1, 123456789012, a), Part(2, 123456822345, b)]


def test_mjpeg_part_without_a_stamp_is_skipped() -> None:
    p = MjpegParser()
    assert list(p.feed(part(b"\xff\xd8X\xff\xd9", None, 0))) == []
    assert list(p.feed(part(b"\xff\xd8Y\xff\xd9", 5, 9))) == [Part(9, 5, b"\xff\xd8Y\xff\xd9")]


def test_dechunk_across_any_split() -> None:
    from conqueror_ai.domain.http import Dechunker

    body = b"part one" + b"\xff\xd8" * 40 + b"end"
    wire = b"8\r\npart one\r\n" + b"%x;ext=1\r\n" % 80 + b"\xff\xd8" * 40 + b"\r\n3\r\nend\r\n0\r\n\r\n"
    for step in (1, 2, 5, 7, len(wire)):
        d = Dechunker()
        out = b"".join(d.feed(wire[i : i + step]) for i in range(0, len(wire), step))
        assert out == body
        assert d.done


def test_dechunk_refuses_a_bad_length_line() -> None:
    from conqueror_ai.domain.http import Dechunker

    with pytest.raises(ValueError):
        Dechunker().feed(b"zz\r\nabc\r\n")
