from __future__ import annotations

import pytest

from measure_lib import (
    MjpegParser,
    fit_inverse_depth,
    gyro_stats,
    match_delays,
    rising_edges,
    sonar_stats,
    unwrap_deg,
)

JPEG_A = b"\xff\xd8AAAA\xff\xd9"
JPEG_B = b"\xff\xd8BB\xff\xd9"


def part(jpeg: bytes) -> bytes:
    return b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg) + jpeg


def test_mjpeg_parser_finds_whole_pictures_across_chunks() -> None:
    stream = part(JPEG_A) + part(JPEG_B)
    p = MjpegParser()
    got: list[bytes] = []
    for i in range(0, len(stream), 7):  # small chunks split markers and pictures
        got += list(p.feed(stream[i : i + 7]))
    assert got == [JPEG_A, JPEG_B]


def test_mjpeg_parser_waits_for_the_end_of_a_picture() -> None:
    p = MjpegParser()
    assert list(p.feed(part(JPEG_A)[:-3])) == []
    assert list(p.feed(part(JPEG_A)[-3:])) == [JPEG_A]


def test_rising_edges_and_delays() -> None:
    t = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
    v = [10, 10, 200, 210, 10, 10, 220]
    edges = rising_edges(t, v, 100)
    assert edges == [0.2, 0.6]
    assert match_delays([0.05, 0.45], edges) == pytest.approx([0.15, 0.15])


def test_a_flash_without_an_edge_is_skipped() -> None:
    assert match_delays([0.0, 5.0], [0.2], max_s=1.0) == pytest.approx([0.2])


def test_depth_fit_recovers_a_perfect_inverse_depth() -> None:
    tape = [0.3, 0.6, 1.0, 2.0]
    rel = [(1 / d - 0.1) / 2.0 for d in tape]  # 1/d = 2 r + 0.1
    fit = fit_inverse_depth(tape, rel)
    assert fit.a == pytest.approx(2.0)
    assert fit.c == pytest.approx(0.1)
    assert all(abs(err) < 1e-6 for _, _, err in fit.rows)


def test_depth_fit_needs_two_points() -> None:
    with pytest.raises(ValueError):
        fit_inverse_depth([1.0], [0.5])


def test_sonar_no_echo_counts_none_and_far() -> None:
    s = sonar_stats([500, 510, None, 4000, 490, 505])
    assert s.samples == 6
    assert s.no_echo_pct == pytest.approx(100 * 2 / 6)
    assert s.median_mm == pytest.approx(502.5)


def test_sonar_with_no_valid_reading() -> None:
    s = sonar_stats([None, None])
    assert s.no_echo_pct == 100 and s.median_mm is None


def test_unwrap_across_180() -> None:
    assert unwrap_deg([179.0, -179.0, -178.0]) == pytest.approx([179.0, 181.0, 182.0])


def test_gyro_drift_per_minute() -> None:
    t = [0.0, 60.0, 120.0]
    g = gyro_stats(t, [0.0, 0.5, 1.0], [0.01, 0.01, 0.01])
    assert g.minutes == pytest.approx(2.0)
    assert g.drift_deg_per_min == pytest.approx(0.5)
    assert g.rate_mean_dps == pytest.approx(0.01)


def test_depth_fit_for_plain_depth() -> None:
    tape = [0.5, 1.0, 2.0]
    rel = [(d - 0.2) / 3.0 for d in tape]  # d = 3 r + 0.2
    fit = fit_inverse_depth(tape, rel, inverse=False)
    assert fit.a == pytest.approx(3.0)
    assert all(abs(err) < 1e-6 for _, _, err in fit.rows)
