"""The drive link's speed conversion and the drive config (AI engine LLD §17.7)."""

from __future__ import annotations

import asyncio
import contextlib
import json
import time
from pathlib import Path
from typing import Any

import pytest
from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import FRESH_FOR_S, MIN_GAP_S, DriveLink, drive_msg, to_percent
from conqueror_ai.domain.drive.curves import Command
from conqueror_ai.domain.drive.limits import Limits

ENGINE = Path(__file__).resolve().parents[1]


def test_speed_to_percent_follows_the_measured_table() -> None:
    assert to_percent(0.0) == 0
    assert to_percent(17.7) == 15  # the lowest smooth speed
    assert to_percent(22.0) == 40
    assert to_percent(25.0) == 56  # between 22 (40 %) and 33.5 (100 %): 55.7
    assert to_percent(40.0) == 100


def test_a_drive_message_is_drive_rate_with_the_turn_rate_plus_left() -> None:
    assert drive_msg(Command(25.0, -12.34)) == {"t": "drive_rate", "v": 56, "yaw_rate": -12.3}


def test_the_drive_config_file_loads_and_checks(tmp_path: Path) -> None:
    lim, table = load_drive(ENGINE / "config" / "drive.yaml")
    assert lim.v_cap_cms == 25.0 and table[1] == (17.7, 15.0)
    bad = tmp_path / "drive.yaml"
    bad.write_text("limits: {top_speed: 3}\n")
    with pytest.raises(ValueError, match="unknown limits top_speed"):
        load_drive(bad)
    bad.write_text("speed_table: [[0, 0], [20, 40], [10, 50]]\n")
    with pytest.raises(ValueError, match="must rise"):
        load_drive(bad)
    assert load_drive(tmp_path / "missing.yaml")[0] == load_drive(ENGINE / "missing.yaml")[0]


@pytest.mark.parametrize(
    "limit",
    [
        "v_cap_cms: 30",
        "v_cruise_cms: 30",
        "v_min_cms: 15",
        "inner_min_cms: 15",
        "spin_dps: 40",
        "r_floor_m: 0.2",
        "clear_m: 0.05",
        "stop_range_m: 0.2",
        "dv_cms_per_s: 30",
        "dw_dps_per_s: 200",
    ],
)
def test_the_drive_config_may_only_make_driving_more_careful(tmp_path: Path, limit: str) -> None:
    bad = tmp_path / "drive.yaml"
    bad.write_text(f"limits: {{{limit}}}\n")
    with pytest.raises(ValueError, match="more careful"):
        load_drive(bad)


# ---- the link, against a stand-in bridge on this machine
class Bridge:
    """A stand-in bridge: keeps what it is sent (with the time), and answers a drive_rate with `answer`."""

    def __init__(self, answer: dict[str, Any] | None = None) -> None:
        self.got: list[tuple[float, dict[str, Any]]] = []
        self.answer = answer

    async def handle(self, ws: ServerConnection) -> None:
        with contextlib.suppress(ConnectionClosed):
            async for raw in ws:
                m = json.loads(raw)
                self.got.append((time.monotonic(), m))
                if m.get("t") in ("drive_rate", "cam_set") and self.answer is not None:
                    await ws.send(json.dumps(self.answer))

    def kinds(self) -> list[str]:
        return [m["t"] for _, m in self.got if m["t"] not in ("hello", "ping")]


async def linked(bridge: Bridge) -> tuple[DriveLink, asyncio.Event, asyncio.Task[None], Server]:
    server = await serve(bridge.handle, "127.0.0.1", 0)
    port = next(iter(server.sockets)).getsockname()[1]
    link, stop = DriveLink("127.0.0.1", port), asyncio.Event()
    task = asyncio.create_task(link.run(stop))
    for _ in range(50):
        if link.connected:
            break
        await asyncio.sleep(0.02)
    return link, stop, task, server


async def unlink(stop: asyncio.Event, task: asyncio.Task[None], server: Server) -> None:
    stop.set()
    task.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await task
    server.close()
    await server.wait_closed()


async def test_finishing_without_having_driven_sends_nothing() -> None:
    bridge = Bridge()
    link, stop, task, server = await linked(bridge)
    await link.cam(0.0, -15.0)
    await link.finish()
    await asyncio.sleep(0.1)
    await unlink(stop, task, server)
    assert bridge.kinds() == ["cam_set"]  # no stop: it would hold a phone driver's tank (bridge R9.6)


async def test_finishing_after_driving_stops_and_hands_control_back() -> None:
    bridge = Bridge()
    link, stop, task, server = await linked(bridge)
    await link.drive(Command(25.0, 10.0))
    await link.finish()
    await asyncio.sleep(0.1)
    await unlink(stop, task, server)
    assert bridge.kinds()[0] == "drive_rate" and bridge.kinds()[-2:] == ["stop", "release_control"]


async def test_the_refresh_stops_when_the_engine_stalls() -> None:
    bridge = Bridge()
    link, stop, task, server = await linked(bridge)
    t0 = time.monotonic()
    await link.drive(Command(25.0, 0.0))
    await asyncio.sleep(1.0)  # no new command: the engine is stuck
    await unlink(stop, task, server)
    sent = [t - t0 for t, m in bridge.got if m["t"] == "drive_rate"]
    assert len(sent) >= 2  # refreshed while the command is fresh ...
    assert max(sent) <= FRESH_FOR_S + 0.1  # ... then left to the bridge's 0.3 s deadman


@pytest.mark.parametrize(
    ("answer", "refused"),
    [
        ({"t": "nack", "code": "B_1", "reason": "INVALID_COMMAND"}, "INVALID_COMMAND"),  # any drive refusal
        ({"t": "nack", "id": 1, "reason": "STOPPED"}, "STOPPED"),
        ({"t": "nack", "id": 1, "reason": "BUSY"}, None),  # a camera move refused while busy: not the end
    ],
)
async def test_a_refusal_ends_the_run_but_a_busy_camera_does_not(
    answer: dict[str, Any], refused: str | None
) -> None:
    bridge = Bridge(answer)
    link, stop, task, server = await linked(bridge)
    if "id" in answer:
        await link.cam(0.0, -15.0)
    else:
        await link.drive(Command(25.0, 0.0))
    await asyncio.sleep(0.15)
    await unlink(stop, task, server)
    assert link.refused == refused


async def test_fast_ticks_stay_under_the_bridges_limit_and_the_newest_command_is_sent() -> None:
    bridge = Bridge()
    link, stop, task, server = await linked(bridge)
    t0 = time.monotonic()
    for i in range(40):  # the loop may tick ~25 times a second (once per picture, and on time-outs)
        await link.drive(Command(25.0, float(i)))
        await asyncio.sleep(0.025)
    await asyncio.sleep(0.15)
    await unlink(stop, task, server)
    sent = [(t - t0, m) for t, m in bridge.got if m["t"] == "drive_rate"]
    per_s = len(sent) / (sent[-1][0] - sent[0][0] + 1e-9)
    assert per_s <= 1 / MIN_GAP_S + 1  # the bridge refuses over 25 a second (RATE_LIMITED, ws_clients.h)
    assert sent[-1][1]["yaw_rate"] == 39.0  # the newest command still goes out


def test_v_min_above_the_slow_speed_is_refused(tmp_path: Path) -> None:  # review minor 9
    f = tmp_path / "drive.yaml"
    f.write_text("limits: {v_min_cms: 20}\n")
    with pytest.raises(ValueError, match="18"):
        load_drive(f)


@pytest.mark.parametrize("limit", ["body_front_m: 0.10", "body_rear_m: 0.08", "body_half_w_m: 0.08"])
def test_the_body_may_only_be_made_bigger(tmp_path: Path, limit: str) -> None:
    f = tmp_path / "drive.yaml"
    f.write_text(f"limits: {{{limit}}}\n")
    with pytest.raises(ValueError, match="more careful"):
        load_drive(f)
    f.write_text("limits: {body_front_m: 0.16, body_half_w_m: 0.12}\n")
    assert load_drive(f)[0].sweep_r_m > Limits().sweep_r_m
