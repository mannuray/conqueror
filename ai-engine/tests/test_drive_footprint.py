"""One body rule (AI engine LLD §18.13.2, Plan Z6): no outline point nearer than the keep-out to a known
obstacle, or nearer than it already is."""

from __future__ import annotations

import math

import numpy as np

from conqueror_ai.domain.drive.costmap import CostMap, CostSettings
from conqueror_ai.domain.drive.curves import STOP, Command, roll_out
from conqueror_ai.domain.drive.footprint import Footprint
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.room_map import RoomMap

LIM = Limits()
F = Footprint(LIM)
AT = (0.0, 0.0, 0.0)


def area(x0: float, y0: float, x1: float, y1: float) -> list[tuple[float, float]]:
    return [
        (float(x), float(y)) for x in np.arange(x0, x1 + 1e-9, 0.025) for y in np.arange(y0, y1 + 1e-9, 0.025)
    ]


def cost_with(marks: list[tuple[float, float]], unseen: list[tuple[float, float]] | None = None) -> CostMap:
    room = RoomMap()
    room.seen[:] = True
    if unseen:
        room.seen.reshape(-1)[room.cells(np.array(unseen, dtype=np.float64))] = False
    if marks:
        c = room.cells(np.array(marks, dtype=np.float64))
        room.hit(c, "low")
        room.hit(c, "low")
        room.seen.reshape(-1)[c] = True
    return CostMap(room.snapshot(0, None, False, True))


def test_the_outline_and_its_moves() -> None:
    assert F.outline[:, 0].max() == LIM.body_front_m and F.outline[:, 0].min() == -LIM.body_rear_m
    assert np.abs(F.outline[:, 1]).max() == LIM.body_half_w_m
    spin = F.spin_poses(AT, left=True)
    assert len(spin) == 18 and math.degrees(spin[-1][2]) == 90.0
    back = F.back_poses(AT, 0.2)
    assert back[-1][0] == -0.2 and len(back) == 8


def test_into_the_keep_out_is_refused_beside_it_is_dear() -> None:
    wall = cost_with(area(0.30, -1.5, 0.35, 1.5))  # its cells from 0.25 m (0.30 sits on a cell edge)
    assert not F.allowed(wall, AT, roll_out(AT, STOP, Command(25.0, 0.0), LIM))
    short = [(0.025, 0.0, 0.0), (0.05, 0.0, 0.0)]  # 5 cm: the front's cell 10 cm from the wall's
    assert F.allowed(wall, AT, short)
    assert F.highest_cost(wall, AT, short) > 0.5


def bed() -> CostMap:
    """Floor run 2: the bed's side panel to the right, its leg ahead-right (cells as the map had them)."""
    return cost_with(area(-0.6, -0.40, 0.10, -0.175) + area(0.075, -0.40, 0.30, -0.15))


def test_run_2_beside_the_bed() -> None:
    b = bed()
    assert not F.allowed(b, AT, F.spin_poses(AT, left=True))  # the rear swings into the panel
    assert not F.allowed(b, AT, F.spin_poses(AT, left=False))  # the front into the leg
    assert not F.allowed(b, AT, roll_out(AT, STOP, Command(25.0, 0.0), LIM))  # past the leg
    assert F.allowed(b, AT, F.back_poses(AT, 0.2))  # back the way it came


def test_run_3_unknown_round_the_tank() -> None:
    c = cost_with(area(0.40, 0.125, 0.45, 0.15), unseen=area(-0.4, -0.4, 0.4, 0.4))
    assert F.allowed(c, AT, F.spin_poses(AT, left=True)) and F.allowed(c, AT, F.spin_poses(AT, left=False))
    assert 0.3 <= F.highest_cost(c, AT, F.spin_poses(AT, left=True)) < 0.95


def test_a_turn_is_checked_its_own_way() -> None:
    """A box behind-right (cell centres 11 cm off the body): turning left keeps 9.1 cm, turning right swings
    the front-right corner to 6.4 cm - inside the 7.5 cm keep-out (exact gaps; Z6 review 12: clear
    margins)."""
    box = cost_with(area(-0.175, -0.275, -0.075, -0.225))  # marks mid-cell: centres x -0.175..-0.075
    assert F.allowed(box, AT, F.spin_poses(AT, left=True))
    assert not F.allowed(box, AT, F.spin_poses(AT, left=False))


def test_already_near_may_move_away_not_nearer() -> None:  # Review Focus 1
    """A wall 3 cm beyond the left side (its cells' centres 6 cm off): along it is allowed, into it isn't."""
    wall = cost_with(area(-1.0, 0.175, 1.5, 0.3))
    assert F.allowed(wall, AT, [(0.025 * k, 0.0, 0.0) for k in range(1, 9)])  # straight along it
    assert not F.allowed(wall, AT, roll_out(AT, STOP, Command(25.0, 44.0), LIM))  # curving in


def test_highest_cost_ahead_only() -> None:
    wall = cost_with(area(0.60, -1.5, 0.65, 1.5))
    path = [(0.05 * k, 0.0, 0.0) for k in range(1, 11)]  # 50 cm toward a wall 60 cm ahead
    assert F.highest_cost(wall, AT, path, ahead_m=0.1) < F.highest_cost(wall, AT, path)


def test_a_thing_under_the_front_edge_never_ends_up_inside_the_body() -> None:  # review I1
    """A point already in an obstacle's cell can't get 'nearer' - so the whole body is checked, not only its
    outline: a one-cell leg under the front edge, driving on 5, 10 or 15 cm is refused."""
    at = (0.02, 0.0, 0.0)
    leg = cost_with([(0.155, 0.0)])
    assert leg.dist_many(F.world(at)).min() == 0.0  # the front edge is in the leg's cell
    for d in (0.05, 0.10, 0.15):
        assert not F.allowed(leg, at, [(0.02 + d, 0.0, 0.0)])
    assert F.allowed(leg, at, F.back_poses(at, 0.1))  # away from it


def test_one_mark_beside_the_body_never_freezes_it() -> None:
    """Floor run 3 (design-notes §171): one false mark 5 cm off the right side, 5 cm behind the centre. The
    map rounds distances to 5.0 / 7.1 / 11.2 cm, so sliding past it at the same gap read as 'nearer' for some
    point and every move was refused. Now the body may not come nearer than its gap now, measured exactly.
    Turning stays refused both ways: the front corner (17.4 cm out) sweeps through it (17.2 cm out) one way,
    the rear corner passes 2.6 cm off it the other."""
    mark = cost_with([(-0.05, -(LIM.body_half_w_m + 0.05))])
    marks = mark.marks_within(-1.0, -1.0, 1.0, 1.0)
    assert F.gaps([AT], marks)[0] < mark.settings.keep_out_m  # already inside the keep-out
    assert F.allowed(mark, AT, [(0.025 * k, 0.0, 0.0) for k in range(1, 5)])  # driving on, past it
    assert F.allowed(mark, AT, F.back_poses(AT, 0.05))  # backing off
    assert not F.allowed(mark, AT, F.spin_poses(AT, left=True))
    assert not F.allowed(mark, AT, F.spin_poses(AT, left=False))


def test_beside_a_mark_still_never_nearer_than_now() -> None:
    """The same mark: sideways into it stays refused; and a turn beside a wall 3.5 cm off the left side, whose
    rear corner would pass 4 mm from it - 5 cm cells read both gaps as 5 cm, 1 cm ones don't."""
    mark = cost_with([(-0.05, -(LIM.body_half_w_m + 0.05))])
    assert not F.allowed(mark, AT, [(0.0, -0.025 * k, 0.0) for k in range(1, 4)])
    wall = cost_with(area(-0.6, 0.15, 0.6, 0.3))
    assert not F.allowed(wall, AT, F.spin_poses(AT, left=False))


def test_no_creeping_nearer_a_little_at_a_time() -> None:  # Z6 review 1
    """A wall 6 cm off the left side (its cells' centres): 4 mm nearer is nearer - with a 5 mm allowance
    measured from each new 'now', ten such steps took the gap from 6.9 to 2.9 cm."""
    wall = cost_with(area(-1.0, 0.175, 1.5, 0.3))
    assert not F.allowed(wall, AT, [(0.0, 0.004, 0.0)])
    assert F.allowed(wall, AT, [(0.025 * k, 0.0, 0.0) for k in range(1, 9)])  # along it: the same gap


def test_a_raised_keep_out_is_kept_exactly() -> None:  # Z6 review 3
    """The keep-out may be raised (more careful): a mark 16 cm off the right side, 2 cm sideways toward it
    ends 14 cm off - inside a 15 cm keep-out, refused (the exact gaps reached only 15 cm out)."""
    room = RoomMap()
    room.seen[:] = True
    c = room.cells(np.array([(0.0, -(LIM.body_half_w_m + 0.18))], dtype=np.float64))
    room.hit(c, "low")
    room.hit(c, "low")
    cost = CostMap(room.snapshot(0, None, False, True), CostSettings(keep_out_m=0.15))
    assert not F.allowed(cost, AT, [(0.0, -0.005 * k, 0.0) for k in range(1, 5)])


def test_a_turn_on_the_spot_sweeps_only_floor_it_has_seen() -> None:
    """Nothing senses beside or behind the tank: its corners turn only over floor the map has seen, as
    backing does - unless nothing else is allowed (owner: option b as a last resort, design-notes §176).
    Floor under the body now, or far off, doesn't count."""
    seen = cost_with([])
    assert F.turn_ok(seen, AT, left=True) and F.turn_ok(seen, AT, left=False)
    # a strip 15-20 cm off the right side: turning right the front-right corner sweeps 17.4 cm out, turning
    # left the rear-right only 14.6 cm
    beside = cost_with([], unseen=area(-0.10, -0.20, 0.10, -0.15))
    assert not F.turn_ok(beside, AT, left=False) and F.turn_ok(beside, AT, left=True)
    under = cost_with([], unseen=area(-0.05, -0.05, 0.05, 0.05))  # beneath the tank
    assert F.turn_ok(under, AT, left=True) and F.turn_ok(under, AT, left=False)
    far = cost_with([], unseen=area(0.35, -0.5, 0.6, 0.5))  # past the corners' reach
    assert F.turn_ok(far, AT, left=True)
    assert F.turn_ok(beside, AT, left=False, seen=False)  # the last resort: the body's rule alone (§176)


def test_a_turn_beside_a_wall_still_keeps_the_body_rule() -> None:
    box = cost_with(area(-0.175, -0.275, -0.075, -0.225))  # test_a_turn_is_checked_its_own_way's box
    assert F.turn_ok(box, AT, left=True) and not F.turn_ok(box, AT, left=False)
    assert not F.turn_ok(box, AT, left=False, seen=False)  # the last resort never relaxes the body's rule
