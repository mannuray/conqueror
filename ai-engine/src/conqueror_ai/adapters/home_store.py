"""The home spot's files (AI engine LLD §20.5): `homes/<name>/home.npz` on the laptop - the kept map layers
and a JSON description (version, frame, runs, things) in one file, replaced in one step (a power cut mid-save
leaves the old home or the new one, never half of each). Git-ignored: it maps the home."""

from __future__ import annotations

import io
import json
import os
import zipfile
from dataclasses import asdict
from pathlib import Path
from typing import Any

import numpy as np

from conqueror_ai.domain.home import HOME_VERSION, Home
from conqueror_ai.domain.memory import Remembered


class HomeError(ValueError):
    """A home that can't be used, said plainly (start fresh or delete the folder)."""


class HomeStore:
    def __init__(self, root: Path) -> None:
        self.root = root

    def _file(self, name: str) -> Path:
        return self.root / name / "home.npz"

    def save(self, name: str, home: Home) -> None:
        about = {
            "version": HOME_VERSION,
            "frame": {
                "kind": "home",
                "pose_offset": [0.0, 0.0, 0.0],
                "cell_m": home.cell_m,
                "size_m": home.size_m,
            },
            "runs": list(home.runs),
            "things": [asdict(t) for t in home.things],
        }
        buf = io.BytesIO()
        np.savez_compressed(
            buf,
            low=home.low,
            high=home.high,
            seen=home.seen,
            about=np.frombuffer(json.dumps(about).encode(), np.uint8),
        )
        path = self._file(name)
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_name(path.name + ".tmp")
        with tmp.open("wb") as f:
            f.write(buf.getvalue())
            f.flush()
            os.fsync(f.fileno())  # on the disk before the rename (minor 2: else empty after a power cut)
        os.replace(tmp, path)  # one step: the old home or the new one

    def load(self, name: str, size_m: float | None = None, cell_m: float | None = None) -> Home | None:
        """The home, or None when there is none; a damaged home, another version or another map size or cell
        size (when given) is a HomeError."""
        path = self._file(name)
        if not path.exists():
            return None
        fix = f"start fresh or delete {path.parent}"
        try:
            with np.load(path) as z:
                about = json.loads(bytes(z["about"]).decode())
                low, high, seen = z["low"], z["high"], z["seen"]
            if not isinstance(about, dict):
                raise TypeError("the description is not a mapping")
            d: dict[str, Any] = about  # pyright: ignore[reportUnknownVariableType]
            version, frame = d.get("version"), d["frame"]
            things = tuple(Remembered(**t) for t in d["things"])
            runs = tuple(str(r) for r in d["runs"])
            home_cell, home_size = float(frame["cell_m"]), float(frame["size_m"])
        except (OSError, ValueError, KeyError, TypeError, AttributeError, zipfile.BadZipFile, EOFError) as e:
            raise HomeError(f"home '{name}': damaged ({type(e).__name__}) - {fix}") from None
        if version != HOME_VERSION:
            raise HomeError(f"home '{name}': version {version}, this engine reads {HOME_VERSION} - {fix}")
        if size_m is not None and home_size != size_m:
            raise HomeError(f"home '{name}': another map size ({home_size} m) - {fix}")
        if cell_m is not None and home_cell != cell_m:
            raise HomeError(f"home '{name}': another cell size ({home_cell} m) - {fix}")
        n = round(home_size / home_cell)
        if not (low.shape == high.shape == seen.shape == (n, n)):
            raise HomeError(f"home '{name}': damaged (its map is not {n} x {n}) - {fix}")
        return Home(low, high, seen, home_cell, home_size, runs, things)
