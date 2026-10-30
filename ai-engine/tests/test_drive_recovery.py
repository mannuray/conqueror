"""The standard ways out (AI engine LLD §18.13.4, Plan Z6): look, turn, back up, give up."""

from __future__ import annotations

import numpy as np

from conqueror_ai.domain.drive.costmap import CostMap
from conqueror_ai.domain.drive.curves import STOP, Command
from conqueror_ai.domain.drive.footprint import Footprint
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.recovery import Recovery
from conqueror_ai.domain.drive.sim import SimTank
from conqueror_ai.domain.room_map import RoomMap

LIM = Limits()
TILT = -15.0
DT = 0.1


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


def pocket() -> list[tuple[float, float]]:
    """Things 17.5 cm each side and 25 cm ahead (cell centres): no turn either way, nothing forward."""
    side = area(-0.6, 0.175, 0.5, 0.3)
    return side + [(x, -y) for x, y in side] + area(0.25, -0.3, 0.5, 0.3)


def run(
    rec: Recovery, cost: CostMap, seconds: float, t0: float = 0.0
) -> tuple[list[tuple[Command, object]], SimTank]:
    """Ticks of one step until it finishes (None) or the time is up; the tank follows the commands."""
    tank, outs = SimTank(), list[tuple[Command, object]]()
    for k in range(round(seconds / DT)):
        o = rec.tick(t0 + k * DT, tank.at, cost, DT)
        if o is None:
            break
        outs.append(o)
        tank.step(o[0], DT)
    return outs, tank


def test_look_pans_and_stands() -> None:
    rec = Recovery(LIM, Footprint(LIM), TILT)
    rec.start(0.0, (0.0, 0.0, 0.0))
    assert rec.step == "look"
    outs, _ = run(rec, cost_with([]), 10.0)
    assert all(c == STOP for c, _ in outs)
    assert [cam for _, cam in outs if cam is not None] == [
        (-90.0, TILT),
        (-45.0, TILT),
        (45.0, TILT),
        (90.0, TILT),
        (0.0, TILT),
    ]
    assert 4.5 <= len(outs) * DT <= 5.5  # 1 s standing at each


def test_the_order_look_turn_back_blocked() -> None:
    """Boxed in, nothing known behind: look, then (no turn allowed) back - not allowed either - blocked."""
    rec = Recovery(LIM, Footprint(LIM), TILT)
    cost = cost_with(pocket(), unseen=area(-0.6, -0.15, -0.1, 0.15))
    steps: list[str] = []
    for k in range(4):
        rec.start(10.0 * k, (0.0, 0.0, 0.0))
        steps.append(rec.step)
        run(rec, cost, 8.0, 10.0 * k)
        if rec.step == "blocked":
            break
    assert steps[:2] == ["look", "turn"] and rec.step == "blocked"


def test_turn_the_allowed_way() -> None:
    box = cost_with(area(-0.30, 0.175, -0.20, 0.30))  # off the left rear corner: turning left swings into it
    rec = Recovery(LIM, Footprint(LIM), TILT)
    rec.start(0.0, (0.0, 0.0, 0.0))
    rec.start(1.0, (0.0, 0.0, 0.0))  # stuck again here: the next step
    assert rec.step == "turn"
    outs, tank = run(rec, box, 5.0, 1.0)
    assert outs and all(c.v_cms == 0 and c.w_dps < 0 for c, _ in outs)
    assert -100.0 <= np.degrees(tank.at[2]) <= -85.0


def test_never_backs_over_unknown_floor() -> None:  # Review Focus 3
    rec = Recovery(LIM, Footprint(LIM), TILT)
    cost = cost_with(pocket(), unseen=area(-0.6, -0.15, -0.1, 0.15))
    for k in range(4):
        rec.start(10.0 * k, (0.0, 0.0, 0.0))
        outs, _ = run(rec, cost, 8.0, 10.0 * k)
        assert all(c.v_cms >= 0 for c, _ in outs)
    assert rec.step == "blocked"


def test_backs_over_known_free_floor_20_cm_at_v_min() -> None:
    rec = Recovery(LIM, Footprint(LIM), TILT)
    cost = cost_with(pocket())  # all seen: behind is known free
    rec.start(0.0, (0.0, 0.0, 0.0))
    rec.start(1.0, (0.0, 0.0, 0.0))
    rec.start(2.0, (0.0, 0.0, 0.0))
    assert rec.step == "back"
    outs, tank = run(rec, cost, 5.0, 2.0)
    assert outs and all(c == Command(-LIM.v_min_cms, 0.0) for c, _ in outs)
    assert -0.22 <= tank.at[0] <= -0.18


def test_backs_off_a_mark_touching_its_front_corner() -> None:  # floor run 2, design-notes §170
    """A mark drawn on the front-right corner after the tank got there (the camera's blind zone), clear floor
    behind: it backs off it - the cell under the body is where the tank already is, not unknown floor."""
    rec = Recovery(LIM, Footprint(LIM), TILT)
    corner = (LIM.body_front_m + 0.01, -LIM.body_half_w_m)  # in the same 5 cm cell as the outline's corner
    cost = cost_with([corner])
    assert cost.lethal[cost.cell_of(LIM.body_front_m - 0.001, -LIM.body_half_w_m + 0.001)]
    for k in range(3):
        rec.start(float(k), (0.0, 0.0, 0.0))
    assert rec.step == "back"
    outs, tank = run(rec, cost, 5.0, 2.0)
    assert outs and all(c == Command(-LIM.v_min_cms, 0.0) for c, _ in outs)
    assert -0.22 <= tank.at[0] <= -0.18


def test_never_backs_over_a_mark_behind() -> None:
    rec = Recovery(LIM, Footprint(LIM), TILT)
    cost = cost_with([(-LIM.body_rear_m - 0.06, 0.0)])  # 6 cm behind the tail
    for k in range(3):
        rec.start(float(k), (0.0, 0.0, 0.0))
    _, tank = run(rec, cost, 5.0, 2.0)
    assert tank.at[0] >= -0.06  # stops short: the body's keep-out kept


def test_progress_resets_the_sequence() -> None:
    rec = Recovery(LIM, Footprint(LIM), TILT)
    rec.start(0.0, (0.0, 0.0, 0.0))
    rec.start(1.0, (0.0, 0.0, 0.0))
    assert rec.step == "turn"
    rec.progress((0.35, 0.0, 0.0))
    rec.start(2.0, (0.35, 0.0, 0.0))
    assert rec.step == "look"


# ---- final review minors (Plan Z6)
def test_backing_counts_from_where_it_starts_backing() -> None:  # review M2
    """Stuck again 25 cm from where the sequence began: it still backs its 20 cm."""
    rec = Recovery(LIM, Footprint(LIM), TILT)
    cost = cost_with([])
    rec.start(0.0, (0.0, 0.0, 0.0))
    rec.start(1.0, (0.25, 0.0, 0.0))
    rec.start(2.0, (0.25, 0.0, 0.0))
    assert rec.step == "back"
    tank, x = SimTank(0.25, 0.0, 0.0), list[float]()
    for k in range(40):
        o = rec.tick(2.0 + k * DT, tank.at, cost, DT)
        if o is None:
            break
        tank.step(o[0], DT)
        x.append(tank.at[0])
    assert 0.03 <= min(x) <= 0.07


def test_backs_as_far_as_the_known_floor_goes() -> None:  # review M3
    """Known floor only 10 cm behind: it backs about that far instead of refusing."""
    unknown = area(-0.6, -0.4, -0.10 - 0.09, 0.4)  # beyond 10 cm behind the rear edge
    rec = Recovery(LIM, Footprint(LIM), TILT)
    cost = cost_with(pocket(), unseen=unknown)
    for k in range(3):
        rec.start(float(k), (0.0, 0.0, 0.0))
    assert rec.step == "back"
    outs, tank = run(rec, cost, 5.0, 2.0)
    assert outs and -0.12 <= tank.at[0] <= -0.04


def test_the_turn_is_measured_from_the_pose() -> None:  # review M4
    """A tank turning slower than told (the ramp, a slipping track) still turns its 90 degrees."""
    box = cost_with(area(-0.30, 0.175, -0.20, 0.30))
    rec = Recovery(LIM, Footprint(LIM), TILT)
    rec.start(0.0, (0.0, 0.0, 0.0))
    rec.start(1.0, (0.0, 0.0, 0.0))
    tank = SimTank()
    for k in range(80):
        o = rec.tick(1.0 + k * DT, tank.at, box, DT)
        if o is None:
            break
        tank.step(Command(o[0].v_cms, o[0].w_dps / 2), DT)  # half the rate
    assert -100.0 <= np.degrees(tank.at[2]) <= -85.0


def test_a_turn_that_doesnt_turn_ends() -> None:
    """Measured from the pose, a turn whose tank doesn't turn (a slipping track) would never end: it gives up
    after TURN_S (about twice what 90 degrees takes)."""
    rec = Recovery(LIM, Footprint(LIM), TILT)
    rec.start(0.0, (0.0, 0.0, 0.0))
    rec.start(1.0, (0.0, 0.0, 0.0))
    ends = [k for k in range(60) if rec.tick(1.0 + k * DT, (0.0, 0.0, 0.0), cost_with([]), DT) is None]
    assert ends and 2.5 <= ends[0] * DT <= 3.5
