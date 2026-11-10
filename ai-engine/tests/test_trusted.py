"""Trusted floor (AI engine LLD §28.4, Plan G2): backing only over floor seen free lately, from near."""

from __future__ import annotations

import numpy as np

from conqueror_ai.domain.drive.trusted import TrustedFloor
from conqueror_ai.domain.room_map import RoomMap
from tests.test_room_map import marks, stand

S = 1_000_000


def behind(x0: float = -0.6, x1: float = 0.0) -> list[tuple[float, float]]:
    """Floor points every 2 cm behind a tank at the origin facing +x (and under it)."""
    return [(float(x), float(y)) for x in np.arange(x0, x1, 0.02) for y in np.arange(-0.2, 0.21, 0.02)]


def seen_behind(t_us: int = 1 * S) -> RoomMap:  # 0 is the map's "never"
    room = RoomMap()
    room.update_picture(marks(floor=behind()), t_us, (0.0, 0.0))
    return room


def test_a_straight_back_over_floor_freed_2_s_ago_is_trusted() -> None:
    assert TrustedFloor.ok(seen_behind(), (0.0, 0.0, 0.0), 0.30, 3 * S)


def test_one_cell_6_s_old_is_not() -> None:
    room = RoomMap()
    pts = behind()
    old = (-0.25, 0.0)
    room.update_picture(marks(floor=[old]), 1 * S, (0.0, 0.0))
    (oc,) = room.cells(np.array([old]))
    rest = [p for p in pts if room.cells(np.array([p]))[0] != oc]
    room.update_picture(marks(floor=rest), 5 * S, (0.0, 0.0))
    assert not TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 7 * S)  # that cell is 6 s old
    assert TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.05, 7 * S)  # a shorter back (+ the coast) stays clear


def test_never_seen_is_not() -> None:
    room = RoomMap()
    room.update_picture(marks(floor=behind(-0.2)), 1 * S, (0.0, 0.0))  # only the first 20 cm behind
    assert not TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 2 * S)


def test_a_low_mark_since_is_not() -> None:
    room = seen_behind()
    room.update_picture(marks(low=[(-0.3, 0.05)]), 2 * S, (0.0, 0.0))
    assert not TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 3 * S)


def test_seen_from_beyond_1_5_m_is_not_trusted() -> None:
    room = RoomMap()
    room.update_picture(marks(floor=behind()), 1 * S, (1.8, 0.0))  # the tank was far ahead when it saw it
    assert not TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 2 * S)


def test_the_heading_turns_the_path() -> None:
    room = seen_behind()  # floor behind (-x) only
    assert TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 2 * S)
    assert not TrustedFloor.ok(room, (0.0, 0.0, np.pi), 0.30, 2 * S)  # facing -x: backing goes to +x


def test_free_times_set_by_floor_and_body_reset_by_a_mark() -> None:
    room = RoomMap()
    c = room.cells(np.array([(0.5, 0.5)]))
    room.update_picture(marks(floor=[(0.5, 0.5)]), 3 * S, None)
    assert room.free_us.reshape(-1)[c][0] == 3 * S
    room.hit(c, "low")
    assert room.free_us.reshape(-1)[c][0] == 0
    stand(room, 0.0, 0.0, 0.0, 1, t0_us=7 * S)
    (o,) = room.cells(np.array([(0.0, 0.0)]))
    assert room.free_us.reshape(-1)[o] == 7 * S


def test_times_from_before_a_bridge_restart_are_not_fresh() -> None:  # final review I2
    room = seen_behind(600 * S)  # seen at 600 s on the old clock
    assert not TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 2 * S)  # the bridge restarted: now 2 s


def test_the_strip_covers_the_coast_after_the_stop() -> None:  # final review M4: ~5 cm more
    room = RoomMap()
    room.update_picture(marks(floor=behind(-0.39)), 1 * S, (0.0, 0.0))  # the rear + 30 cm only, no more
    assert not TrustedFloor.ok(room, (0.0, 0.0, 0.0), 0.30, 2 * S)
    assert TrustedFloor.ok(seen_behind(), (0.0, 0.0, 0.0), 0.30, 2 * S)
