"""Recording and replaying a session (AI engine LLD §9).

A session folder holds `telemetry.jsonl` (one line per WebSocket message: {"recv_s": t, "msg": "<raw JSON>"}),
`frames.bin` (per picture: a JSON header line {"seq", "cap_us", "recv_s", "len"} then the JPEG bytes) and
`session.json` (what was recorded and from where). Recordings show the home: keep them on the laptop
(`recordings/` is git-ignored).
"""

from __future__ import annotations

import heapq
import json
import time
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import IO, Any

from conqueror_ai.domain.types import Frame


class SessionRecorder:
    def __init__(self, folder: Path, about: dict[str, Any]) -> None:
        folder.mkdir(parents=True, exist_ok=False)  # never write over an earlier session
        self.folder = folder
        self._telem: IO[str] = (folder / "telemetry.jsonl").open("w", encoding="utf-8")
        self._frames: IO[bytes] = (folder / "frames.bin").open("wb")
        self.telem_count = 0
        self.frame_count = 0
        (folder / "session.json").write_text(json.dumps({**about, "started": time.time()}, indent=1))

    def telemetry(self, recv_s: float, raw: str) -> None:
        self._telem.write(json.dumps({"recv_s": recv_s, "msg": raw}) + "\n")
        self.telem_count += 1

    def frame(self, f: Frame) -> None:
        head = {"seq": f.seq, "cap_us": f.cap_us, "recv_s": f.recv_s, "len": len(f.jpeg)}
        self._frames.write(json.dumps(head).encode() + b"\n" + f.jpeg)
        self.frame_count += 1

    def close(self) -> None:
        self._telem.close()
        self._frames.close()


@dataclass(frozen=True)
class Event:
    """One recorded input in time order: a WebSocket text or a picture."""

    recv_s: float
    text: str | None = None
    frame: Frame | None = None


def read_frames(path: Path) -> Iterator[Frame]:
    with path.open("rb") as fh:
        while True:
            line = fh.readline()
            if not line:
                return
            try:
                h = json.loads(line)
                size = int(h["len"])
                head = (int(h["seq"]), int(h["cap_us"]), float(h["recv_s"]))
            except (ValueError, KeyError, TypeError):
                return  # cut off inside the header line (a crash or power loss while recording)
            jpeg = fh.read(size)
            if len(jpeg) != size:
                return  # cut off at the end (recording stopped mid-write)
            yield Frame(head[0], head[1], head[2], jpeg)


def read_telemetry(path: Path) -> Iterator[tuple[float, str]]:
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if line.strip():
                try:
                    row = json.loads(line)
                    yield float(row["recv_s"]), str(row["msg"])
                except (ValueError, KeyError, TypeError):
                    return  # cut off inside the last line


def replay(folder: Path) -> Iterator[Event]:
    """Every recorded input in the order it arrived (deterministic: same folder, same order). Both files are
    written in arrival order, so they are merged as they are read - a long session is never loaded whole."""
    texts = (Event(t, text=m) for t, m in read_telemetry(folder / "telemetry.jsonl"))
    frames = (Event(f.recv_s, frame=f) for f in read_frames(folder / "frames.bin"))
    yield from heapq.merge(texts, frames, key=lambda e: (e.recv_s, e.frame is not None))
