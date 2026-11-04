"""HomeStore: homes/<name>/ on the laptop (AI engine LLD §20.5; Plan Z8 Review Focus 2)."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import numpy as np
import pytest

from conqueror_ai.adapters.home_store import HomeError, HomeStore
from conqueror_ai.domain.home import Home
from conqueror_ai.domain.memory import Remembered, ThingMemory
from conqueror_ai.domain.places import Place
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


def test_a_malformed_home_is_refused_plainly(tmp_path: Path) -> None:
    """Plan Z8 final review minor 1: a home of the right zip shape but wrong content (a missing key, an
    unknown field, wrong arrays) is a HomeError too, never another exception."""
    import io
    import json

    store = HomeStore(tmp_path)
    good = a_home()
    bad_abouts: list[object] = [
        {"version": 1, "runs": [], "things": []},  # no frame
        {
            "version": 1,
            "frame": {"cell_m": 0.05, "size_m": 8.0},
            "runs": [],
            "things": [{"word": "x", "odd": 1}],
        },
        [1, 2, 3],
    ]
    for about in bad_abouts:
        buf = io.BytesIO()
        raw = np.frombuffer(json.dumps(about).encode(), np.uint8)
        np.savez_compressed(buf, low=good.low, high=good.high, seen=good.seen, about=raw)
        (tmp_path / "h").mkdir(exist_ok=True)
        (tmp_path / "h" / "home.npz").write_bytes(buf.getvalue())
        with pytest.raises(HomeError):
            store.load("h")
    store.save("h", Home(good.low[:10, :10], good.high, good.seen, 0.05, 8.0, good.runs, ()))  # wrong shape
    with pytest.raises(HomeError):
        store.load("h")


def test_store_round_trip_places(tmp_path: Path) -> None:
    store = HomeStore(tmp_path)
    room = RoomMap()
    room.seen[:5, :5] = True
    home = Home.from_run(room, ThingMemory(), None, "r1", places=[Place(3, "toy box", 0.5, -1.25)])
    store.save("h", home)
    got = store.load("h")
    assert got is not None and got.places == (Place(3, "toy box", 0.5, -1.25),)


def test_store_loads_version_1(tmp_path: Path) -> None:
    """A home saved before names (version 1, no places) loads with none."""
    store = HomeStore(tmp_path)
    store.save("h", a_home())
    path = tmp_path / "h" / "home.npz"
    with np.load(path) as z:
        arrays = {k: z[k] for k in z.files}
    about = json.loads(bytes(arrays["about"]).decode())
    about["version"] = 1
    del about["places"]
    arrays["about"] = np.frombuffer(json.dumps(about).encode(), np.uint8)
    np.savez_compressed(path, **arrays)
    got = store.load("h")
    assert got is not None and got.places == () and got.things == a_home().things
