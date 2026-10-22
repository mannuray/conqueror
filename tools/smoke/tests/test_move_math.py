"""Plan K: the maths of move.py (bridge LLD R7.7)."""

from __future__ import annotations

import threading
import time
from typing import Any

import pytest

from move import Session, counts_travelled, mean_cpm, new_cpm


def test_counts_travelled_forward_and_reverse() -> None:
    # the Uno counts rising edges: its counts go up in reverse too (final review C1)
    assert counts_travelled((1000, 2000), (8560, 9560)) == 7560
    assert counts_travelled((1000, 2000), (4780, 5780)) == 3780


def test_new_cpm_from_tape() -> None:
    assert new_cpm(7560, 100.0) == 7560
    assert new_cpm(7560, 98.5) == 7675  # went less far than counted -> more counts per metre
    assert new_cpm(7642, 101.2) == 7551


def test_new_cpm_rejects_bad_measure() -> None:
    with pytest.raises(ValueError):
        new_cpm(7560, 0.0)


def test_mean_cpm() -> None:
    assert mean_cpm([7675, 7660, 7690]) == 7675
    with pytest.raises(ValueError):
        mean_cpm([])


class FakeWs:
    """Stands in for bridge_echo.WsClient: records what is sent; a calib save is answered
    like the bridge since Plan O - "confirm" first, then "confirmed" (the tank's button)
    or "confirm_expired"."""

    def __init__(self, expire: bool = False) -> None:
        self.sent: list[dict[str, Any]] = []
        self.lock = threading.Lock()
        self.expire = expire
        self.pending: list[dict[str, Any]] = []

    def send(self, obj: dict[str, Any]) -> None:
        with self.lock:
            self.sent.append(obj)

    def recv(self, timeout: float) -> dict[str, Any] | None:
        time.sleep(min(timeout, 0.05))
        with self.lock:
            if self.pending:
                return self.pending.pop(0)
            for m in self.sent:
                saving = "counts_per_m" in m or "gyro_scale" in m
                if m.get("t") == "calib" and saving and not m.get("_answered"):
                    m["_answered"] = True
                    end = "confirm_expired" if self.expire else "confirmed"
                    self.pending.append({"t": end, "what": "calib"})
                    return {"t": "confirm", "what": "calib", "secs": 30}
        return None

    def close(self) -> None:
        pass


def test_ask_keeps_the_connection_alive() -> (
    None
):  # final review I1(a): the bridge drops a client silent 3 s
    ws = FakeWs()
    s = Session(ws=ws)

    def slow_reader(_prompt: str) -> str:
        time.sleep(2.5)
        return "98.5"

    assert s.ask("measured?", slow_reader) == "98.5"
    assert sum(1 for m in ws.sent if m.get("t") == "ping") >= 2


def test_save_retakes_control_first() -> (
    None
):  # final review I1(b): control is handed back 5 s after the move
    ws = FakeWs()
    s = Session(ws=ws)
    assert s.save_counts_per_m(7675)
    kinds = [m.get("t") for m in ws.sent]
    i_drive = kinds.index("drive")
    i_calib = max(i for i, k in enumerate(kinds) if k == "calib")
    assert i_drive < i_calib
    drive = ws.sent[i_drive]
    assert drive["v"] == 0 and drive["w"] == 0


def test_ask_passes_eof_on() -> (
    None
):  # review 2 I2: a closed stdin must end the tool, not drive again
    s = Session(ws=FakeWs())

    def closed(_prompt: str) -> str:
        raise EOFError

    with pytest.raises(EOFError):
        s.ask("press Enter", closed)


def test_save_waits_for_the_tanks_button() -> None:  # Plan O (design-notes §91)
    s = Session(ws=FakeWs())
    assert s.save_counts_per_m(7675)


def test_save_not_confirmed_is_not_saved() -> None:
    s = Session(ws=FakeWs(expire=True))
    assert not s.save_counts_per_m(7675)


def test_gyro_scale_save_sends_its_key_and_waits_for_the_button() -> None:  # bridge LLD R19
    ws = FakeWs()
    s = Session(ws=ws)
    assert s.save_gyro_scale(10230)
    calib = [m for m in ws.sent if m.get("t") == "calib"]
    assert calib[-1]["gyro_scale"] == 10230
    assert "counts_per_m" not in calib[-1]


def test_gyro_scale_not_confirmed_is_not_saved() -> None:
    assert not Session(ws=FakeWs(expire=True)).save_gyro_scale(10230)
