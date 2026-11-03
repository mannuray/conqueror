"""CostMaps: one cost map per snapshot and key, the play area always in it (LLD §19.3, review F6)."""

from __future__ import annotations

import numpy as np

from conqueror_ai.domain.depth import Marks
from conqueror_ai.domain.drive.costmaps import CostMaps
from conqueror_ai.domain.room_map import MapSnapshot, RoomMap
from conqueror_ai.domain.types import Pose


def snap() -> MapSnapshot:
    r = RoomMap()
    r.seen[:] = True
    z = np.zeros((0, 2))
    r.update_picture(Marks(z, z, z, z), 0)
    return r.snapshot(0, Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0), False, True)


def test_one_build_per_snapshot_and_key() -> None:
    cm = CostMaps(snap(), None, None)
    assert cm.base() is cm.base()
    assert cm.ignoring(1.0, 0.0, 0.12) is cm.ignoring(1.0, 0.0, 0.12)
    assert cm.ignoring(1.0, 0.0, 0.12) is not cm.base()


def test_the_area_is_always_applied() -> None:
    cm = CostMaps(snap(), None, (0.0, 0.0, 1.0))
    assert cm.area == (0.0, 0.0, 1.0)
    for c in (cm.base(), cm.ignoring(0.5, 0.0, 0.12)):
        ij = c.cell_of(1.5, 0.0)
        assert ij is not None and bool(c.lethal[ij])
        inside = c.cell_of(0.5, 0.0)
        assert inside is not None and not bool(c.lethal[inside])
