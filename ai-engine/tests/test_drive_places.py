"""Places relative to the tank, in the map frame (AI engine LLD §21.2)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.domain.drive.places import to_map


@pytest.mark.parametrize(
    ("heading_deg", "want"),
    [
        (0.0, (1.5, 0.3)),  # heading 0 is along +x, left is +y: 50 cm ahead, 20 cm left of (1.0, 0.1)
        (90.0, (0.8, 0.6)),  # facing +y: ahead is +y, left is -x
        (-90.0, (1.2, -0.4)),  # facing -y: ahead is -y, left is +x
    ],
)
def test_to_map_headings(heading_deg: float, want: tuple[float, float]) -> None:
    x, y = to_map(1.0, 0.1, math.radians(heading_deg), 50.0, 20.0)
    assert x == pytest.approx(want[0], abs=1e-9) and y == pytest.approx(want[1], abs=1e-9)
