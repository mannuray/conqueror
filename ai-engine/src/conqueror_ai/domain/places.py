"""Names on the map (AI engine LLD §23.3, design-notes §211): the owner taps a red or amber block on the ROOM
tab and names it ("bed"). A name is a point and a word, not tied to the cells, so a block that fades or grows
keeps its name where it was. Names are labels and words for "where" only - no skill reads them, nothing drives
to a name except GO THERE, which is the page's own SETXY to the spot `go_spot` picks. Pure: no I/O."""

from __future__ import annotations

import math
import re
from collections.abc import Sequence
from dataclasses import dataclass

import numpy as np
from numpy.typing import NDArray

from conqueror_ai.domain.person_guard import PERSON_WORDS
from conqueror_ai.domain.room_map import MapSnapshot
from conqueror_ai.domain.types import Pose

TAP_M = 0.15  # a tap names the block whose nearest cell is this close (to the cell's edge)
MAX_BLOCK = 4000  # cells: a block is at most this big (the flood fill stops)
MAX_PLACES = 50
MAX_BYTES = 24
GO_NEAR_M = 0.30  # GO THERE: the spot's distance to its block, from ...
GO_FAR_M = 0.45  # ... to
GO_CLEAR_M = 0.30  # and at least this far from any other block
NEAR_M = 1.0  # where words: "near the bed" within this
_NAME = re.compile(r"^[a-z][a-z -]*$")

NO_BLOCK = "Tap a red or amber block to name it."
BAD_NAME = "Names are letters, spaces and - (up to 24)."
NO_PEOPLE = "I don't name people."
FULL = "That's 50 names - take one off first."
NO_SUCH = "I can't find that name."


@dataclass(frozen=True)
class Place:
    id: int
    name: str
    x_m: float
    y_m: float


@dataclass(frozen=True)
class Refusal:
    msg: str


def name_check(name: str) -> str | Refusal:
    """The cleaned name (trimmed, one space between words, lower case), or why not."""
    clean = " ".join(name.split()).lower()
    if not clean or len(clean.encode()) > MAX_BYTES or not _NAME.match(clean):
        return Refusal(BAD_NAME)
    if any(part in PERSON_WORDS for part in clean.replace("-", " ").split()):
        return Refusal(NO_PEOPLE)
    return clean


Cells = NDArray[np.intp]  # (k, 2): ix, iy


def _centres(cells: Cells, cell_m: float, n: int) -> NDArray[np.float64]:
    return (cells.astype(np.float64) - n / 2 + 0.5) * cell_m


def _block_at(snap: MapSnapshot, x: float, y: float) -> Cells | None:
    """The blocked cells joined (8 neighbours) to the blocked cell nearest (x, y) within TAP_M of its edge."""
    n, cell = snap.blocked.shape[0], snap.cell_m
    reach = math.ceil((TAP_M + cell) / cell)
    cx, cy = math.floor(x / cell + n / 2), math.floor(y / cell + n / 2)
    i0, i1, j0, j1 = max(0, cx - reach), min(n, cx + reach + 1), max(0, cy - reach), min(n, cy + reach + 1)
    if i0 >= i1 or j0 >= j1:
        return None
    near = np.argwhere(snap.blocked[i0:i1, j0:j1]) + np.array([i0, j0])
    if near.size == 0:
        return None
    d = np.hypot(*(_centres(near, cell, n) - np.array([x, y])).T)
    k = int(np.argmin(d))
    if d[k] > TAP_M + cell / 2:
        return None
    seen = {(int(near[k, 0]), int(near[k, 1]))}
    todo = list(seen)
    while todo and len(seen) < MAX_BLOCK:
        i, j = todo.pop()
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                a, b = i + di, j + dj
                if 0 <= a < n and 0 <= b < n and (a, b) not in seen and snap.blocked[a, b]:
                    seen.add((a, b))
                    todo.append((a, b))
    return np.array(sorted(seen), np.intp)


def _min_dist(points: NDArray[np.float64], to: NDArray[np.float64]) -> NDArray[np.float64]:
    """For each point, the distance to the nearest of `to` (in slices: a big block stays small in memory)."""
    out = np.full(len(points), np.inf)
    for s in range(0, len(to), 512):
        t = to[s : s + 512]
        d = np.hypot(points[:, None, 0] - t[None, :, 0], points[:, None, 1] - t[None, :, 1]).min(axis=1)
        out = np.minimum(out, d)
    return out


class Places:
    def __init__(self, places: Sequence[Place] = ()) -> None:
        self._places: list[Place] = list(places)
        self.changed = False  # any change since the caller last cleared it

    def all(self) -> list[Place]:
        return list(self._places)

    def name_at(self, snap: MapSnapshot, x: float, y: float, name: str) -> Place | Refusal:
        clean = name_check(name)
        if isinstance(clean, Refusal):
            return clean
        if len(self._places) >= MAX_PLACES:
            return Refusal(FULL)
        block = _block_at(snap, x, y)
        if block is None:
            return Refusal(NO_BLOCK)
        n, cell = snap.blocked.shape[0], snap.cell_m
        mx, my = _centres(block, cell, n).mean(axis=0)
        mi, mj = math.floor(mx / cell + n / 2), math.floor(my / cell + n / 2)
        if not np.any((block[:, 0] == mi) & (block[:, 1] == mj)):  # the middle is off the block (an L)
            c = _centres(block, cell, n)
            mx, my = c[int(np.argmin(np.hypot(c[:, 0] - mx, c[:, 1] - my)))]
        p = Place(
            max((q.id for q in self._places), default=0) + 1, clean, round(float(mx), 2), round(float(my), 2)
        )
        self._places.append(p)
        self.changed = True
        return p

    def rename(self, id: int, name: str) -> Place | Refusal:
        clean = name_check(name)
        if isinstance(clean, Refusal):
            return clean
        for k, q in enumerate(self._places):
            if q.id == id:
                self._places[k] = p = Place(id, clean, q.x_m, q.y_m)
                self.changed = True
                return p
        return Refusal(NO_SUCH)

    def unname(self, id: int) -> bool:
        before = len(self._places)
        self._places = [q for q in self._places if q.id != id]
        self.changed |= len(self._places) != before
        return len(self._places) != before

    def go_spot(self, snap: MapSnapshot, place: Place, pose: Pose | None) -> tuple[float, float] | None:
        """A spot on floor seen this run, GO_NEAR_M..GO_FAR_M from the name's block and GO_CLEAR_M from any
        other, nearest the tank (no pose: nearest the name); None if the block faded or there is none."""
        block = _block_at(snap, place.x_m, place.y_m)
        if block is None:
            return None
        n, cell = snap.blocked.shape[0], snap.cell_m
        pad = math.ceil(GO_FAR_M / cell) + 1
        i0, j0 = max(0, int(block[:, 0].min()) - pad), max(0, int(block[:, 1].min()) - pad)
        i1, j1 = min(n, int(block[:, 0].max()) + pad + 1), min(n, int(block[:, 1].max()) + pad + 1)
        win = np.s_[i0:i1, j0:j1]
        offset = np.array([i0, j0])
        floor = np.argwhere(snap.seen[win] & ~snap.blocked[win]) + offset
        if floor.size == 0:
            return None
        pts = _centres(floor, cell, n)
        d = _min_dist(pts, _centres(block, cell, n))
        ok = (d >= GO_NEAR_M) & (d <= GO_FAR_M)
        mine = np.zeros((n, n), np.bool_)
        mine[block[:, 0], block[:, 1]] = True
        clear = math.ceil(GO_CLEAR_M / cell) + 1
        k0, l0 = max(0, i0 - clear), max(0, j0 - clear)
        k1, l1 = min(n, i1 + clear), min(n, j1 + clear)
        others = np.argwhere(snap.blocked[k0:k1, l0:l1] & ~mine[k0:k1, l0:l1]) + np.array([k0, l0])
        if others.size and ok.any():
            ok &= _min_dist(pts, _centres(others, cell, n)) >= GO_CLEAR_M
        if not ok.any():
            return None
        to = (pose.x_m, pose.y_m) if pose is not None else (place.x_m, place.y_m)
        cand = pts[ok]
        best = cand[int(np.argmin(np.hypot(cand[:, 0] - to[0], cand[:, 1] - to[1])))]
        return round(float(best[0]), 2), round(float(best[1]), 2)


def where_words(
    x: float, y: float, places: Sequence[Place], things: Sequence[tuple[str, float, float]], home: bool
) -> str:
    """'near the bed' (the nearest name or other thing within NEAR_M), else the distance from home or the
    start."""
    near = [(math.dist((x, y), (p.x_m, p.y_m)), p.name) for p in places]
    near += [(math.dist((x, y), (tx, ty)), w) for w, tx, ty in things]
    if near:
        d, word = min(near)
        if d <= NEAR_M:
            return f"near the {word}"
    return f"{math.hypot(x, y):.1f} m from {'home' if home else 'the start'}"
