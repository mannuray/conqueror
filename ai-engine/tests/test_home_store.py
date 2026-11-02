"""HomeStore: homes/<name>/ on the laptop (AI engine LLD §20.5; Plan Z8 Review Focus 2)."""

from __future__ import annotations

import subprocess
from pathlib import Path

import numpy as np
import pytest

from conqueror_ai.adapters.home_store import HomeError, HomeStore
from conqueror_ai.domain.home import Home
from conqueror_ai.domain.memory import Remembered, ThingMemory
from conqueror_ai.domain.room_map import RoomMap


def a_home(run: str = "r1") -> Home:
    room = RoomMap()
    room.low.reshape(-1)[room.cells(np.array([(1.0, 0.0)]))] = 4.0
    room.seen[:5, :5] = True
    mem = ThingMemory([Remembered("ball", 1.2, 0.4, 100.0, run)])
    return Home.from_run(room, mem, None, run)


def test_a_home_round_trips(tmp_path: Path) -> None:
    store, home = HomeStore(tmp_path), a_home()
    store.save("kitchen", home)
    got = store.load("kitchen")
    assert got is not None
    assert np.array_equal(got.low, home.low) and np.array_equal(got.seen, home.seen)
    assert got.runs == home.runs and got.things == home.things and (got.cell_m, got.size_m) == (0.05, 8.0)


def test_no_home_is_none(tmp_path: Path) -> None:
    assert HomeStore(tmp_path).load("nowhere") is None


def test_another_version_or_size_is_refused(tmp_path: Path) -> None:
    store = HomeStore(tmp_path)
    home = a_home()
    store.save(
        "h", Home(home.low[:80, :80], home.high[:80, :80], home.seen[:80, :80], 0.05, 4.0, home.runs, ())
    )
    with pytest.raises(HomeError, match=r"home 'h'.*map size"):
        store.load("h", size_m=8.0, cell_m=0.05)


def test_a_half_written_home_is_refused_plainly(tmp_path: Path) -> None:
    """Power off during a save: a cut-off file is refused with a plain message, never another exception; a
    leftover temporary file doesn't spoil the good home."""
    store = HomeStore(tmp_path)
    store.save("h", a_home())
    (tmp_path / "h" / "home.npz.tmp").write_bytes(b"half")  # a save cut off before its rename
    assert store.load("h") is not None
    data = (tmp_path / "h" / "home.npz").read_bytes()
    (tmp_path / "h" / "home.npz").write_bytes(data[: len(data) // 2])
    with pytest.raises(HomeError, match=r"home 'h'.*damaged"):
        store.load("h")


def test_homes_are_git_ignored() -> None:
    here = Path(__file__).resolve().parents[1]
    r = subprocess.run(["git", "check-ignore", "-q", str(here / "homes" / "x")], cwd=here, check=False)
    assert r.returncode == 0
