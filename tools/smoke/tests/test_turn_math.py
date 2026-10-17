"""Plan L: the path maths of turn.py (bridge LLD R8.5)."""

from __future__ import annotations

import math

from turn import arc_point, integrate


def test_integrate_straight() -> None:
    samples = [(i * 751, 0.0) for i in range(11)]
    x, y = integrate(samples, 75.1)
    assert abs(x - 100.0) < 0.01 and abs(y) < 0.01


def test_integrate_quarter_circle_left() -> None:
    r, n, cpc = 60.0, 900, 75.1
    length = r * math.pi / 2
    samples = [(round(i * length * cpc / n), i * 90.0 / n) for i in range(n + 1)]
    x, y = integrate(samples, cpc)
    assert abs(x - 60.0) < 1.0 and abs(y - 60.0) < 1.0


def test_arc_point() -> None:
    x, y = arc_point(60.0, 90.0)
    assert abs(x - 60.0) < 1e-9 and abs(y - 60.0) < 1e-9
    x, y = arc_point(60.0, -90.0)
    assert abs(x - 60.0) < 1e-9 and abs(y + 60.0) < 1e-9
