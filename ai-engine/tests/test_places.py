"""Names on the map, the GO THERE spot and where words (AI engine LLD §23.3)."""

from __future__ import annotations

import math
from collections.abc import Iterable

import numpy as np

from conqueror_ai.domain.places import (
    GO_CLEAR_M,
    GO_FAR_M,
    GO_NEAR_M,
    Place,
    Places,
    Refusal,
    name_check,
    where_words,
)
from conqueror_ai.domain.room_map import MapSnapshot
from conqueror_ai.domain.types import Pose

N, CELL = 160, 0.05


def ij(x: float, y: float) -> tuple[int, int]:
    return int(np.floor(x / CELL + N / 2)), int(np.floor(y / CELL + N / 2))


def centre(i: int, j: int) -> tuple[float, float]:
    return (i - N / 2 + 0.5) * CELL, (j - N / 2 + 0.5) * CELL


def box(x0: float, x1: float, y0: float, y1: float) -> list[tuple[int, int]]:
    """The cells whose centres lie in [x0, x1) x [y0, y1)."""
    i0, i1 = ij(x0, 0)[0], ij(x1, 0)[0]
    j0, j1 = ij(0, y0)[1], ij(0, y1)[1]
    return [(i, j) for i in range(i0, i1) for j in range(j0, j1)]


def snap(blocks: Iterable[tuple[int, int]], seen: Iterable[tuple[int, int]] = ()) -> MapSnapshot:
    b = np.zeros((N, N), np.bool_)
    s = np.zeros((N, N), np.bool_)
    for c in blocks:
        b[c] = True
    for c in seen:
        s[c] = True
    return MapSnapshot(0, None, CELL, b, np.zeros((N, N), np.bool_), s, False, True, True)


BLOCK = box(0.8, 1.2, -0.2, 0.2)  # a 40 x 40 cm block round (1, 0)
ALL_FLOOR = box(-1.0, 2.5, -1.5, 1.5)


def dist_to(cells: list[tuple[int, int]], p: tuple[float, float]) -> float:
    return min(math.dist(centre(*c), p) for c in cells)


def test_name_at_near_block() -> None:
    p = Places().name_at(snap(BLOCK), 1.0, 0.30, "bed")  # the block's edge is 0.10 m away
    assert isinstance(p, Place)
    assert p.name == "bed" and abs(p.x_m - 1.0) < 0.03 and abs(p.y_m) < 0.03


def test_name_at_too_far() -> None:
    r = Places().name_at(snap(BLOCK), 1.0, 0.40, "bed")  # 0.20 m away
    assert r == Refusal("Tap a red or amber block to name it.")


def test_name_l_shape() -> None:
    cells = box(0.0, 1.0, 0.0, 0.1) + box(0.9, 1.0, 0.1, 1.0)  # an L: its mean is off the block
    p = Places().name_at(snap(cells), 0.5, 0.05, "sofa")
    assert isinstance(p, Place)
    assert ij(p.x_m, p.y_m) in set(cells)


def test_name_rules() -> None:
    for bad in ["", "   ", "x" * 25, "bed2", "person", "kids", "my baby"]:
        assert isinstance(name_check(bad), Refusal), bad
    assert name_check("person") == Refusal("I don't name people.")
    assert name_check("  Toy   Box ") == "toy box"
    assert name_check("old-chair") == "old-chair"


def test_rename_unname() -> None:
    ps = Places()
    p = ps.name_at(snap(BLOCK), 1.0, 0.0, "bed")
    assert isinstance(p, Place)
    ps.changed = False
    r = ps.rename(p.id, "Cot")
    assert isinstance(r, Place) and r.name == "cot" and ps.changed
    assert ps.rename(99, "x") == Refusal("I can't find that name.")
    assert ps.unname(p.id) and ps.all() == [] and not ps.unname(p.id)


def test_fifty_one() -> None:
    ps = Places([Place(i, f"thing {chr(97 + i % 26)}", 0.0, 0.0) for i in range(50)])
    assert ps.name_at(snap(BLOCK), 1.0, 0.0, "bed") == Refusal("That's 50 names - take one off first.")


def test_same_name_twice() -> None:
    ps = Places()
    a = ps.name_at(snap(BLOCK), 1.0, 0.0, "chair")
    b = ps.name_at(snap(BLOCK), 1.0, 0.0, "chair")
    assert isinstance(a, Place) and isinstance(b, Place) and a.id != b.id and len(ps.all()) == 2


def test_go_spot_open_floor() -> None:
    sn = snap(BLOCK, ALL_FLOOR)
    ps = Places()
    p = ps.name_at(sn, 1.0, 0.0, "box")
    assert isinstance(p, Place)
    g = ps.go_spot(sn, p, Pose(0, -0.5, 0.0, 0.0, 0.0, 0.0))
    assert g is not None
    d = dist_to(BLOCK, g)
    assert GO_NEAR_M - 1e-6 <= d <= GO_FAR_M + 1e-6
    assert g[0] < 0.8  # on the tank's side


def test_go_spot_unseen_side() -> None:
    seen = [c for c in ALL_FLOOR if centre(*c)[0] > 0.85]  # nothing seen on the tank's (-x) side
    sn = snap(BLOCK, seen)
    ps = Places()
    p = ps.name_at(sn, 1.0, 0.0, "box")
    assert isinstance(p, Place)
    g = ps.go_spot(sn, p, Pose(0, -0.5, 0.0, 0.0, 0.0, 0.0))
    assert g is not None and g[0] > 0.85


def test_go_spot_none() -> None:
    sn = snap(BLOCK)  # no floor seen round it
    ps = Places()
    p = ps.name_at(sn, 1.0, 0.0, "box")
    assert isinstance(p, Place)
    assert ps.go_spot(sn, p, Pose(0, -0.5, 0.0, 0.0, 0.0, 0.0)) is None
    assert ps.go_spot(snap([], ALL_FLOOR), p, Pose(0, -0.5, 0.0, 0.0, 0.0, 0.0)) is None  # the block faded


def test_go_spot_clear_of_others() -> None:
    other = box(0.3, 0.4, -0.2, 0.2)  # another block between the tank and the box
    sn = snap(BLOCK + other, ALL_FLOOR)
    ps = Places()
    p = ps.name_at(sn, 1.0, 0.0, "box")
    assert isinstance(p, Place)
    g = ps.go_spot(sn, p, Pose(0, -0.5, 0.0, 0.0, 0.0, 0.0))
    assert g is not None
    assert dist_to(other, g) >= GO_CLEAR_M - 1e-6


def test_where_near_name() -> None:
    assert where_words(1.0, 0.5, [Place(1, "bed", 1.0, 0.0)], [], True) == "near the bed"


def test_where_near_thing() -> None:
    assert where_words(1.0, 0.5, [], [("chair", 1.2, 0.5)], True) == "near the chair"


def test_where_nearest_of_two() -> None:
    w = where_words(1.0, 0.0, [Place(1, "bed", 1.8, 0.0)], [("chair", 1.3, 0.0)], True)
    assert w == "near the chair"


def test_where_far_home() -> None:
    assert where_words(1.2, 0.0, [Place(1, "bed", 3.0, 0.0)], [], True) == "1.2 m from home"


def test_where_far_start() -> None:
    assert where_words(0.0, -2.0, [], [], False) == "2.0 m from the start"
