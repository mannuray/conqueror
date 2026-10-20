# AI Engine Phase 0 — Measurements Implementation Plan (Plan Q0)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure what module 1 of the AI engine can rely on — picture age and rate per size, the Neural Engine budget, depth accuracy against a tape, ultrasonic no-echo on soft and slanted things, gyro drift at rest, camera height and blind zone — and record the numbers that set the engine's parameters.

**Architecture:** A small measurement project `tools/measure/` (Python 3.12, uv) with the pure calculations in `measure_lib.py` (tested), one script per measurement, and the existing `tools/smoke/td_record.py` for telemetry recordings. Nothing in this plan moves the tank; the only command sent besides `hello` / `ping` / `debug` is `camera_set` (picture size).

**Tech Stack:** Python 3.12 / uv, numpy (< 2.3), OpenCV, Ultralytics YOLO26n exported to CoreML (torch 2.7), coremltools 9, Apple's CoreML Depth Anything V2 Small (Hugging Face), `websockets` (sync client), pytest, ruff, pyright strict.

**Spec:** `docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md` §9 phase 0 (approved), with `docs/lld/ai-engine-lld.md` §11 (approved) and `docs/research/2026-10-20-module1-mapping.md` ("to measure on the tank").

**Branch:** `plan-q0` from `main`. Task 4 only with the owner present (the tank stands still; nothing drives).

## Global Constraints

- Measurement code is kept as scripts under `tools/measure/`; the numbers go to design-notes; nothing here becomes engine code.
- **Nothing moves the tank:** no `drive`, `turn`, `move`, `logo_run`; only `hello`, `ping`, `debug`, `camera_set`.
- Pictures show the home: `tools/measure/out/` is git-ignored, and pictures are deleted after the numbers are recorded; never copy them elsewhere.
- Never print WiFi SSIDs or passwords; the bridge has no authentication (home network, development only).
- Python: type hints, ruff + pyright strict clean, `uv` only.
- Pins (found while drafting): **torch 2.7** and **numpy < 2.3** — newer ones break coremltools 9's YOLO export.
- Commits: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Rules this plan decides (owner confirms at plan review)

1. **Picture age** is measured with a screen flash: a full-screen window turns white once a second and the script times the first bright picture from the stream — the age on arrival at the laptop, end to end, with no bridge change.
2. The **OV3660 readout time** (for the half-readout correction) is **not** measured here: it needs the bridge's frame time stamps (Plan Q1); `readout_us` starts at half a frame interval.
3. **Depth vs tape:** the centre patch of Depth Anything against a box at 0.3 / 0.6 / 1.0 / 2.0 m, fitted both as inverse depth and as plain depth (Apple's output is named "depth"; the better fit shows which it is), once on wood and once on the rug.
4. Gyro drift and ultrasonic no-echo come from `td` recordings made with the existing `tools/smoke/td_record.py` at 50 Hz.

## Review Focus

1. **A stream that stops or never starts** (wrong address, the phone and another viewer already watching — the stream allows two) → the script says so and ends; no hang.
2. **The flash window behind other windows or the camera not facing the screen** → "no flash seen", not a wrong delay.
3. **`camera_set` left on a different size** → `latency.py` puts the first size back at the end (also after Ctrl-C would be nice; it is not required).
4. **A `td_record` CSV with faults or missing distances** → counted as no echo, never a crash.
5. **Pictures of the home** → only under `out/` (git-ignored); deleted after Task 5.

## File map

| Task | Creates | Changes |
|---|---|---|
| 1 Project + pure helpers | `tools/measure/{pyproject.toml,.gitignore,measure_lib.py}`, `tools/measure/tests/test_measure_lib.py` | — |
| 2 Neural Engine budget (laptop only) | `tools/measure/ne_budget.py` | — |
| 3 Tank scripts | `tools/measure/{bridge.py,latency.py,depth_tape.py,td_stats.py,snap.py}` | — |
| 4 On the tank (owner) | — | — |
| 5 Record | — | `docs/design-notes.md`, `docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md` §7 if a target changes, `CLAUDE.md` |

---

### Task 1: The measurement project and its pure helpers

**Files:**
- Create: `tools/measure/pyproject.toml`, `tools/measure/.gitignore`, `tools/measure/measure_lib.py`
- Test: `tools/measure/tests/test_measure_lib.py`

**Interfaces:**
- Produces: `MjpegParser.feed(bytes) -> Iterator[bytes]`; `rising_edges(times, values, threshold) -> list[float]`; `match_delays(flashes, edges, max_s=1.0) -> list[float]`; `fit_inverse_depth(tape_m, rel, inverse=True) -> DepthFit(a, c, rows[(tape, model, err %)])`; `sonar_stats(dist_mm, max_mm=4000) -> SonarStats(samples, no_echo_pct, median_mm, spread_mm)`; `unwrap_deg(values)`; `gyro_stats(t_s, yaw_deg, rate_dps) -> GyroStats(minutes, drift_deg_per_min, rate_mean_dps, rate_std_dps)`.

- [ ] **Step 1: The project** — `tools/measure/pyproject.toml`:

```toml
[project]
name = "conqueror-measure"
version = "0.1.0"
description = "Phase 0 measurements for the AI engine module 1 (Spec 7 §9)"
requires-python = ">=3.12,<3.13"
dependencies = [
  "numpy>=1.26,<2.3",  # coremltools 9 turns 1-element arrays into numbers; numpy 2.3+ refuses
  "opencv-python>=4.10",
  "ultralytics>=8.3",
  "coremltools>=8.0",
  "huggingface_hub>=0.25",
  "websockets>=13",
  "torch>=2.7,<2.8",  # coremltools 9 is tested up to torch 2.7; newer torch breaks the YOLO CoreML export
  "torchvision>=0.22,<0.23",
]

[tool.uv]
package = false

[dependency-groups]
dev = ["ruff>=0.7", "pyright>=1.1", "pytest>=8"]

[tool.pytest.ini_options]
pythonpath = ["."]
testpaths = ["tests"]

[tool.ruff]
line-length = 110
target-version = "py312"
extend-exclude = ["models"]

[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "SIM", "RUF"]

[tool.pyright]
pythonVersion = "3.12"
typeCheckingMode = "strict"
exclude = ["models", ".venv"]
reportMissingTypeStubs = false
```

`tools/measure/.gitignore`:

```text
.venv/
models/
out/
recordings/
__pycache__/
```

Run: `cd tools/measure && uv sync`
Expected: resolves and installs (first time ≈ 1-2 GB with torch); `uv run python -c "import torch, numpy, coremltools; print(torch.__version__, numpy.__version__)"` → `2.7.x 2.2.x`.

- [ ] **Step 2: The failing tests** — `tools/measure/tests/test_measure_lib.py`:

```python
from __future__ import annotations

import pytest

from measure_lib import (
    MjpegParser,
    fit_inverse_depth,
    gyro_stats,
    match_delays,
    rising_edges,
    sonar_stats,
    unwrap_deg,
)

JPEG_A = b"\xff\xd8AAAA\xff\xd9"
JPEG_B = b"\xff\xd8BB\xff\xd9"


def part(jpeg: bytes) -> bytes:
    return b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg) + jpeg


def test_mjpeg_parser_finds_whole_pictures_across_chunks() -> None:
    stream = part(JPEG_A) + part(JPEG_B)
    p = MjpegParser()
    got: list[bytes] = []
    for i in range(0, len(stream), 7):  # small chunks split markers and pictures
        got += list(p.feed(stream[i : i + 7]))
    assert got == [JPEG_A, JPEG_B]


def test_mjpeg_parser_waits_for_the_end_of_a_picture() -> None:
    p = MjpegParser()
    assert list(p.feed(part(JPEG_A)[:-3])) == []
    assert list(p.feed(part(JPEG_A)[-3:])) == [JPEG_A]


def test_rising_edges_and_delays() -> None:
    t = [0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6]
    v = [10, 10, 200, 210, 10, 10, 220]
    edges = rising_edges(t, v, 100)
    assert edges == [0.2, 0.6]
    assert match_delays([0.05, 0.45], edges) == pytest.approx([0.15, 0.15])


def test_a_flash_without_an_edge_is_skipped() -> None:
    assert match_delays([0.0, 5.0], [0.2], max_s=1.0) == pytest.approx([0.2])


def test_depth_fit_recovers_a_perfect_inverse_depth() -> None:
    tape = [0.3, 0.6, 1.0, 2.0]
    rel = [(1 / d - 0.1) / 2.0 for d in tape]  # 1/d = 2 r + 0.1
    fit = fit_inverse_depth(tape, rel)
    assert fit.a == pytest.approx(2.0)
    assert fit.c == pytest.approx(0.1)
    assert all(abs(err) < 1e-6 for _, _, err in fit.rows)


def test_depth_fit_needs_two_points() -> None:
    with pytest.raises(ValueError):
        fit_inverse_depth([1.0], [0.5])


def test_sonar_no_echo_counts_none_and_far() -> None:
    s = sonar_stats([500, 510, None, 4000, 490, 505])
    assert s.samples == 6
    assert s.no_echo_pct == pytest.approx(100 * 2 / 6)
    assert s.median_mm == pytest.approx(502.5)


def test_sonar_with_no_valid_reading() -> None:
    s = sonar_stats([None, None])
    assert s.no_echo_pct == 100 and s.median_mm is None


def test_unwrap_across_180() -> None:
    assert unwrap_deg([179.0, -179.0, -178.0]) == pytest.approx([179.0, 181.0, 182.0])


def test_gyro_drift_per_minute() -> None:
    t = [0.0, 60.0, 120.0]
    g = gyro_stats(t, [0.0, 0.5, 1.0], [0.01, 0.01, 0.01])
    assert g.minutes == pytest.approx(2.0)
    assert g.drift_deg_per_min == pytest.approx(0.5)
    assert g.rate_mean_dps == pytest.approx(0.01)


def test_depth_fit_for_plain_depth() -> None:
    tape = [0.5, 1.0, 2.0]
    rel = [(d - 0.2) / 3.0 for d in tape]  # d = 3 r + 0.2
    fit = fit_inverse_depth(tape, rel, inverse=False)
    assert fit.a == pytest.approx(3.0)
    assert all(abs(err) < 1e-6 for _, _, err in fit.rows)
```

Run: `cd tools/measure && uv run pytest -q`
Expected: FAIL — `ModuleNotFoundError: No module named 'measure_lib'`.

- [ ] **Step 3: The helpers** — `tools/measure/measure_lib.py`:

```python
"""Phase 0 measurements - the pure parts (tested in tests/test_measure_lib.py, no tank needed)."""

from __future__ import annotations

import statistics
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass


class MjpegParser:
    """Splits a multipart MJPEG byte stream into JPEG pictures (the bridge's `:81/stream`)."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> Iterator[bytes]:
        """Adds bytes; yields every complete JPEG (from FFD8 to FFD9) found so far."""
        self._buf += chunk
        while True:
            start = self._buf.find(b"\xff\xd8")
            if start < 0:
                del self._buf[:-1]  # keep a possible first half of a marker
                return
            end = self._buf.find(b"\xff\xd9", start + 2)
            if end < 0:
                del self._buf[:start]
                return
            yield bytes(self._buf[start : end + 2])
            del self._buf[: end + 2]


def rising_edges(times: Sequence[float], values: Sequence[float], threshold: float) -> list[float]:
    """Times where the value goes from below `threshold` to at or above it (screen turned white)."""
    out: list[float] = []
    for i in range(1, len(values)):
        if values[i - 1] < threshold <= values[i]:
            out.append(times[i])
    return out


def match_delays(flashes: Sequence[float], edges: Sequence[float], max_s: float = 1.0) -> list[float]:
    """For each flash, the delay to the first edge after it (within `max_s`); unmatched flashes skipped."""
    out: list[float] = []
    j = 0
    for f in flashes:
        while j < len(edges) and edges[j] < f:
            j += 1
        if j < len(edges) and edges[j] - f <= max_s:
            out.append(edges[j] - f)
            j += 1
    return out


@dataclass(frozen=True)
class DepthFit:
    a: float
    c: float
    rows: list[tuple[float, float, float]]  # (tape m, model m, error %)


def fit_inverse_depth(tape_m: Sequence[float], rel: Sequence[float], inverse: bool = True) -> DepthFit:
    """Least-squares fit 1/d = a*r + c (Depth Anything gives relative inverse depth r), or d = a*r + c with
    inverse=False (if a model's output is plain depth), with the error of the fitted distance per mark."""
    if len(tape_m) != len(rel) or len(tape_m) < 2:
        raise ValueError("need at least two (tape, relative) pairs")
    ys = [1.0 / d if inverse else d for d in tape_m]
    mx, my = statistics.fmean(rel), statistics.fmean(ys)
    sxx = sum((x - mx) ** 2 for x in rel)
    if sxx == 0:
        raise ValueError("all relative values are equal")
    a = sum((x - mx) * (y - my) for x, y in zip(rel, ys, strict=True)) / sxx
    c = my - a * mx
    rows: list[tuple[float, float, float]] = []
    for d, r in zip(tape_m, rel, strict=True):
        y = a * r + c
        model = (1.0 / y if y > 0 else float("inf")) if inverse else y
        rows.append((d, model, (model - d) / d * 100.0))
    return DepthFit(a, c, rows)


@dataclass(frozen=True)
class SonarStats:
    samples: int
    no_echo_pct: float
    median_mm: float | None
    spread_mm: float | None  # 10-90 % range of the valid readings


def sonar_stats(dist_mm: Iterable[int | None], max_mm: int = 4000) -> SonarStats:
    """`None`, a fault or anything at / over `max_mm` counts as "no echo"."""
    all_ = list(dist_mm)
    valid = sorted(d for d in all_ if d is not None and 0 < d < max_mm)
    if not all_:
        return SonarStats(0, 0.0, None, None)
    no_echo = 100.0 * (len(all_) - len(valid)) / len(all_)
    if not valid:
        return SonarStats(len(all_), no_echo, None, None)
    p10, p90 = valid[int(0.1 * (len(valid) - 1))], valid[int(0.9 * (len(valid) - 1))]
    return SonarStats(len(all_), no_echo, float(statistics.median(valid)), float(p90 - p10))


@dataclass(frozen=True)
class GyroStats:
    minutes: float
    drift_deg_per_min: float  # heading change per minute while standing still
    rate_mean_dps: float  # the gyro's reading at rest (its bias)
    rate_std_dps: float


def unwrap_deg(values: Sequence[float]) -> list[float]:
    """Heading without the jumps at +-180."""
    out: list[float] = []
    off = 0.0
    for i, v in enumerate(values):
        if i and v - values[i - 1] > 180:
            off -= 360
        elif i and v - values[i - 1] < -180:
            off += 360
        out.append(v + off)
    return out


def gyro_stats(t_s: Sequence[float], yaw_deg: Sequence[float], rate_dps: Sequence[float]) -> GyroStats:
    if len(t_s) < 2 or t_s[-1] <= t_s[0]:
        raise ValueError("need a recording longer than zero seconds")
    yaw = unwrap_deg(yaw_deg)
    minutes = (t_s[-1] - t_s[0]) / 60.0
    return GyroStats(
        minutes,
        (yaw[-1] - yaw[0]) / minutes,
        statistics.fmean(rate_dps),
        statistics.pstdev(rate_dps),
    )
```

- [ ] **Step 4: Run the tests and the checks**

Run: `cd tools/measure && uv run pytest -q && uv run ruff check . && uv run ruff format --check . && uv run pyright`
Expected: `11 passed`, ruff clean, `0 errors`.

- [ ] **Step 5: Commit**

```bash
git add tools/measure/pyproject.toml tools/measure/uv.lock tools/measure/.gitignore tools/measure/measure_lib.py tools/measure/tests
git commit -m "feat(measure): phase 0 measurement project and its pure helpers

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The Neural Engine budget (laptop only)

**Files:**
- Create: `tools/measure/ne_budget.py`

**Interfaces:**
- Produces: `yolo_coreml(name, imgsz)`, `depth_coreml() -> MLModel` (input `image` 518×392, output `depth`), used by Task 3's `depth_tape.py`.

- [ ] **Step 1: The script** — `tools/measure/ne_budget.py`:

```python
"""Phase 0: how fast YOLO and Depth Anything run on this Mac, alone and together (Spec 7 §9, LLD §11).

No tank needed. Exports YOLO to CoreML (once), fetches Apple's CoreML Depth Anything V2 Small (once),
then times each model on the same picture, and both one after the other (as the engine's single worker).

    uv run python ne_budget.py [--yolo yolo26n] [--imgsz 320] [--seconds 10]
"""

from __future__ import annotations

import argparse
import statistics
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

import coremltools as ct  # type: ignore[import-untyped]
from huggingface_hub import snapshot_download  # type: ignore[import-untyped]
from PIL import Image
from ultralytics import YOLO  # type: ignore[import-untyped]
from ultralytics.utils import ASSETS  # type: ignore[import-untyped]

MODELS = Path(__file__).parent / "models"


def yolo_coreml(name: str, imgsz: int) -> Any:
    """The CoreML export of a YOLO model at this input size (exported once, then reused)."""
    target = MODELS / f"{name}-{imgsz}.mlpackage"
    if not target.exists():
        MODELS.mkdir(exist_ok=True)
        out = YOLO(f"{name}.pt").export(format="coreml", imgsz=imgsz, nms=False, half=True)
        Path(out).rename(target)
    return YOLO(str(target), task="detect")


def depth_coreml() -> Any:
    folder = snapshot_download(
        "apple/coreml-depth-anything-v2-small",
        local_dir=MODELS / "da2s",
        allow_patterns=["*F16.mlpackage/*"],
    )
    return ct.models.MLModel(
        str(Path(folder) / "DepthAnythingV2SmallF16.mlpackage"), compute_units=ct.ComputeUnit.ALL
    )


def timed(fn: Callable[[], object], seconds: float) -> list[float]:
    """Milliseconds per call, after 5 warm-up calls, for about `seconds`."""
    for _ in range(5):
        fn()
    out: list[float] = []
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        t0 = time.perf_counter()
        fn()
        out.append((time.perf_counter() - t0) * 1000)
    return out


def summary(label: str, ms: list[float]) -> str:
    med = statistics.median(ms)
    p90 = sorted(ms)[int(len(ms) * 0.9) - 1]
    return f"{label:<28} median {med:6.1f} ms  p90 {p90:6.1f} ms  -> {1000 / med:5.1f} per second"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--yolo", default="yolo26n")
    ap.add_argument("--imgsz", type=int, default=320)
    ap.add_argument("--seconds", type=float, default=10)
    a = ap.parse_args()
    picture = Image.open(ASSETS / "bus.jpg").convert("RGB")
    yolo = yolo_coreml(a.yolo, a.imgsz)
    depth = depth_coreml()
    small = picture.resize((518, 392))

    def run_yolo() -> object:
        return yolo.predict(picture, imgsz=a.imgsz, verbose=False)

    def run_depth() -> object:
        return depth.predict({"image": small})

    def run_both() -> object:
        run_yolo()
        return run_depth()

    print(summary(f"{a.yolo} @ {a.imgsz}", timed(run_yolo, a.seconds)))
    print(summary("Depth Anything V2 Small", timed(run_depth, a.seconds)))
    print(summary("both, one after the other", timed(run_both, a.seconds)))


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it at 320 and 640 px**

Run: `cd tools/measure && uv run python ne_budget.py --seconds 8 && uv run python ne_budget.py --imgsz 640 --seconds 8`
Expected (measured while drafting on the owner's M1 16 GB; ± 20 %):
```
yolo26n @ 320                median    4.4 ms  ...  -> ~230 per second
Depth Anything V2 Small      median   32.7 ms  ...  ->  ~30 per second
both, one after the other    median   41.2 ms  ...  ->  ~24 per second
yolo26n @ 640                median   10.6 ms  ...  ->  ~94 per second
both, one after the other    median   46.2 ms  ...  ->  ~22 per second
```
The first run exports `models/yolo26n-320.mlpackage` / `-640` and downloads the depth model into `models/da2s` (git-ignored). A `Torch version 2.7.1 has not been tested` warning is expected.

- [ ] **Step 3: Checks and commit**

Run: `cd tools/measure && uv run ruff check . && uv run pyright`
Expected: clean, `0 errors`.

```bash
git add tools/measure/ne_budget.py
git commit -m "feat(measure): Neural Engine budget - YOLO and Depth Anything on CoreML

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: The tank scripts

**Files:**
- Create: `tools/measure/bridge.py`, `tools/measure/latency.py`, `tools/measure/depth_tape.py`, `tools/measure/td_stats.py`, `tools/measure/snap.py`

**Interfaces:**
- Consumes: `measure_lib` (Task 1), `depth_coreml` (Task 2); the bridge's `:81/stream`, `/ws` (`hello`, `ping`, `debug`, `camera_set`; `td` replies), and `tools/smoke/td_record.py`'s CSV columns (`t_s`, `yaw`, `rate`, `dist_mm`, `dist_fault`).
- Produces: the five command-line tools of Task 4.

- [ ] **Step 1: The bridge client** — `tools/measure/bridge.py`:

```python
"""Phase 0: talking to the bridge - the camera stream and the WebSocket. Read-only except `camera_set`.

The bridge has no authentication (home network, development only). Nothing here moves the tank.
"""

from __future__ import annotations

import http.client
import json
import threading
import time
from collections.abc import Iterator
from typing import Any

from websockets.sync.client import ClientConnection, connect

from measure_lib import MjpegParser

DEFAULT_HOST = "192.168.31.167"  # the tank on the home network (mDNS does not resolve from Python here)


def frames(host: str, port: int = 81) -> Iterator[tuple[float, bytes]]:
    """(laptop receive time, JPEG) for every picture of the camera stream, newest as they come."""
    conn = http.client.HTTPConnection(host, port, timeout=5)
    conn.request("GET", "/stream")
    resp = conn.getresponse()
    if resp.status != 200:
        raise RuntimeError(f"stream answered {resp.status}")
    parser = MjpegParser()
    while True:
        chunk = resp.read1(65536)
        if not chunk:
            return
        now = time.monotonic()
        for jpeg in parser.feed(chunk):
            yield now, jpeg


class Bridge:
    """A WebSocket viewer: says hello, keeps pinging (the bridge drops a silent client after 3 s) and
    keeps the newest `td` sample. It never takes the drive."""

    def __init__(self, host: str) -> None:
        self.ws: ClientConnection = connect(f"ws://{host}/ws", open_timeout=5)
        self.td: dict[str, Any] | None = None
        self.replies: list[dict[str, Any]] = []
        self._lock = threading.Lock()
        self._stop = False
        self.send({"t": "hello", "client": "measure", "proto": 1})
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._pinger, daemon=True).start()

    def send(self, msg: dict[str, Any]) -> None:
        self.ws.send(json.dumps(msg))

    def debug(self, hz: int = 50) -> None:
        self.send({"t": "debug", "on": True, "hz": hz})

    def camera_size(self, size: str) -> None:
        """QVGA / VGA / SVGA / HD - the bridge keeps it (Settings -> Tank -> Camera shows it too)."""
        self.send({"t": "camera_set", "size": size, "id": 1})

    def latest_td(self) -> dict[str, Any] | None:
        with self._lock:
            return self.td

    def close(self) -> None:
        self._stop = True
        try:
            self.send({"t": "debug", "on": False})
        finally:
            self.ws.close()

    def _reader(self) -> None:
        for raw in self.ws:
            msg = json.loads(raw)
            with self._lock:
                if msg.get("t") == "td":
                    self.td = msg
                elif msg.get("t") in ("ack", "nack", "tank"):
                    self.replies.append(msg)

    def _pinger(self) -> None:
        while not self._stop:
            try:
                self.send({"t": "ping", "t0": int(time.time() * 1000)})
            except Exception:
                return
            time.sleep(1.0)
```

- [ ] **Step 2: Picture age and rate** — `tools/measure/latency.py`:

```python
"""Phase 0: how old a camera picture is when it reaches the laptop, and pictures per second per size.

Point the tank's camera at this laptop's screen (about 30 cm away, the screen filling the picture). The
script turns a full-screen window white for 0.3 s once a second and watches the stream for the picture
getting bright: the delay from "white" to "bright picture arrived" is the picture's age on arrival.
Nothing moves. Runs each picture size in turn (it changes the size in the bridge's camera settings and
puts the first one back at the end).

    uv run python latency.py [--host 192.168.31.167] [--sizes QVGA,VGA] [--seconds 20]
"""

from __future__ import annotations

import argparse
import statistics
import threading
import time

import cv2
import numpy as np

from bridge import DEFAULT_HOST, Bridge, frames
from measure_lib import match_delays, rising_edges


def watch(host: str, seconds: float, out: list[tuple[float, float]]) -> None:
    """(receive time, centre brightness) of every picture for `seconds`."""
    end = time.monotonic() + seconds
    for t, jpeg in frames(host):
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_GRAYSCALE)
        if img is not None:  # pyright: ignore[reportUnnecessaryComparison] - None for a broken picture
            h, w = img.shape
            out.append((t, float(img[h // 3 : 2 * h // 3, w // 3 : 2 * w // 3].mean())))
        if t > end:
            return


def run_size(host: str, size: str, seconds: float) -> None:
    samples: list[tuple[float, float]] = []
    reader = threading.Thread(target=watch, args=(host, seconds + 1, samples), daemon=True)
    reader.start()
    flashes: list[float] = []
    cv2.namedWindow("flash", cv2.WND_PROP_FULLSCREEN)
    cv2.setWindowProperty("flash", cv2.WND_PROP_FULLSCREEN, cv2.WINDOW_FULLSCREEN)
    black, white = np.zeros((90, 160), np.uint8), np.full((90, 160), 255, np.uint8)
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        cv2.imshow("flash", white)
        cv2.waitKey(1)
        flashes.append(time.monotonic())
        cv2.waitKey(300)
        cv2.imshow("flash", black)
        cv2.waitKey(700)
    cv2.destroyWindow("flash")
    reader.join(timeout=5)
    if len(samples) < 10:
        print(f"{size}: too few pictures ({len(samples)}) - is the stream reachable?")
        return
    times = [t for t, _ in samples]
    values = [v for _, v in samples]
    threshold = (min(values) + max(values)) / 2
    delays = match_delays(flashes, rising_edges(times, values, threshold))
    fps = (len(times) - 1) / (times[-1] - times[0])
    if not delays:
        print(f"{size}: {fps:.1f} pictures/s; no flash seen - point the camera at the screen")
        return
    print(
        f"{size}: {fps:.1f} pictures/s; age on arrival median {statistics.median(delays) * 1000:.0f} ms, "
        f"min {min(delays) * 1000:.0f}, max {max(delays) * 1000:.0f} ({len(delays)} flashes)"
    )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--sizes", default="QVGA,VGA")
    ap.add_argument("--seconds", type=float, default=20)
    a = ap.parse_args()
    bridge = Bridge(a.host)
    sizes = a.sizes.split(",")
    try:
        for size in sizes:
            bridge.camera_size(size)
            time.sleep(2)  # the camera restarts with the new size
            run_size(a.host, size, a.seconds)
        bridge.camera_size(sizes[0])
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Depth against the tape** — `tools/measure/depth_tape.py`:

```python
"""Phase 0: Depth Anything against a tape measure (and the ultrasonic), on one floor.

Put a box (at least 20 cm wide and tall) straight ahead of the tank at each tape distance; press Enter for
each. The script grabs a picture and the ultrasonic distance, takes the depth model's value in the middle
of the picture, then fits 1/d = a*r + c over all marks and prints the error of every mark. Run it once on
the wood floor and once on the rug (--floor). Nothing moves. Pictures are kept in out/ (git-ignored).

    uv run python depth_tape.py --floor wood [--marks 0.3,0.6,1.0,2.0]
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from bridge import DEFAULT_HOST, Bridge, frames
from measure_lib import fit_inverse_depth
from ne_budget import depth_coreml

OUT = Path(__file__).parent / "out"


def one_picture(host: str) -> bytes:
    for _, jpeg in frames(host):
        return jpeg  # the first whole picture
    raise RuntimeError("no picture from the stream")


def centre_value(model: object, jpeg: bytes) -> float:
    rgb = cv2.cvtColor(cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR), cv2.COLOR_BGR2RGB)
    pic = Image.fromarray(rgb).resize((518, 392))
    depth = np.asarray(model.predict({"image": pic})["depth"], dtype=np.float32)  # type: ignore[attr-defined]
    h, w = depth.shape
    return float(np.median(depth[h // 2 - 20 : h // 2 + 20, w // 2 - 20 : w // 2 + 20]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--floor", required=True)
    ap.add_argument("--marks", default="0.3,0.6,1.0,2.0")
    a = ap.parse_args()
    marks = [float(m) for m in a.marks.split(",")]
    model = depth_coreml()
    bridge = Bridge(a.host)
    bridge.debug(10)
    OUT.mkdir(exist_ok=True)
    rel: list[float] = []
    sonar: list[float | None] = []
    try:
        for m in marks:
            input(f"Box at {m:.2f} m straight ahead, then Enter ")
            time.sleep(0.5)
            jpeg = one_picture(a.host)
            (OUT / f"depth-{a.floor}-{m:.2f}.jpg").write_bytes(jpeg)
            rel.append(centre_value(model, jpeg))
            td = bridge.latest_td()
            d = td.get("dist_mm") if td else None
            sonar.append(d / 1000 if isinstance(d, int | float) else None)
    finally:
        bridge.close()
    for inverse in (True, False):  # which way round the model's output is shows in the smaller errors
        fit = fit_inverse_depth(marks, rel, inverse)
        print(f"floor {a.floor}: {'1/d' if inverse else 'd'} = {fit.a:.4f} r + {fit.c:.4f}")
        print("tape m | depth model m | error % | ultrasonic m")
        for (tape, model_m, err), s in zip(fit.rows, sonar, strict=True):
            print(f"{tape:6.2f} | {model_m:13.2f} | {err:7.1f} | {s if s is not None else 'no echo'}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 4: Gyro and ultrasonic from a recording** — `tools/measure/td_stats.py`:

```python
"""Phase 0: gyro drift and ultrasonic no-echo rate from a `td` recording (tools/smoke/td_record.py CSV).

uv run python td_stats.py gyro  rest.csv        # tank standing still, 5 minutes
uv run python td_stats.py sonar sofa.csv        # tank facing a surface, 10 seconds
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

from measure_lib import gyro_stats, sonar_stats


def column(rows: list[dict[str, str]], name: str) -> list[float]:
    return [float(r[name]) for r in rows]


def dist(r: dict[str, str]) -> int | None:
    if r.get("dist_fault") in ("1", "True", "true") or r.get("dist_mm") in (None, "", "null", "None"):
        return None
    return int(float(r["dist_mm"]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("what", choices=["gyro", "sonar"])
    ap.add_argument("csv", type=Path)
    a = ap.parse_args()
    with a.csv.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if a.what == "gyro":
        g = gyro_stats(column(rows, "t_s"), column(rows, "yaw"), column(rows, "rate"))
        print(
            f"{g.minutes:.1f} min at rest: heading drift {g.drift_deg_per_min:+.2f} deg/min, "
            f"rate {g.rate_mean_dps:+.3f} +- {g.rate_std_dps:.3f} deg/s"
        )
    else:
        s = sonar_stats(dist(r) for r in rows)
        med = f"{s.median_mm:.0f} mm" if s.median_mm is not None else "-"
        spread = f"{s.spread_mm:.0f} mm" if s.spread_mm is not None else "-"
        print(f"{s.samples} readings: no echo {s.no_echo_pct:.0f} %, median {med}, spread (10-90 %) {spread}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Live picture and snapshots** — `tools/measure/snap.py`:

```python
"""Phase 0: show the live camera picture with a centre line and save a picture on Enter (mount and blind
zone). Nothing moves. Pictures go to out/ (git-ignored: they show the home). Esc or q ends.

    uv run python snap.py [--host 192.168.31.167]
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

import cv2
import numpy as np

from bridge import DEFAULT_HOST, frames

OUT = Path(__file__).parent / "out"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default=DEFAULT_HOST)
    a = ap.parse_args()
    OUT.mkdir(exist_ok=True)
    for _, jpeg in frames(a.host):
        img = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if img is None:  # pyright: ignore[reportUnnecessaryComparison] - None for a broken picture
            continue
        h, w = img.shape[:2]
        shown = img.copy()
        cv2.line(shown, (w // 2, 0), (w // 2, h), (0, 255, 0), 1)
        cv2.line(shown, (0, h - 1), (w, h - 1), (0, 0, 255), 2)  # the bottom row: the blind zone starts here
        cv2.imshow("tank camera (Enter = save, q = quit)", shown)
        key = cv2.waitKey(1) & 0xFF
        if key in (13, 10):
            name = OUT / f"snap-{time.strftime('%H%M%S')}.jpg"
            name.write_bytes(jpeg)
            print(f"saved {name}")
        elif key in (27, ord("q")):
            break
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
```

- [ ] **Step 6: Checks, and a smoke run against a stand-in stream**

Run: `cd tools/measure && uv run ruff check . && uv run ruff format --check . && uv run pyright && uv run pytest -q`
Expected: clean, `0 errors`, `11 passed`.

Smoke (no tank): serve a stand-in MJPEG stream of black and white pictures on `127.0.0.1:8181` and run `latency.watch` on it, then `depth_tape.centre_value` on Ultralytics' `bus.jpg`:
```bash
cd tools/measure && uv run python - <<'EOF'
import threading, time, cv2, numpy as np
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
pics = [cv2.imencode(".jpg", np.full((240, 320), v, np.uint8))[1].tobytes() for v in (0, 255)]
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Type", "multipart/x-mixed-replace;boundary=cqframe"); self.end_headers()
        for i in range(40):
            j = pics[(i // 5) % 2]
            self.wfile.write(b"\r\n--cqframe\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(j) + j); self.wfile.flush(); time.sleep(0.05)
    def log_message(self, *a): pass
srv = ThreadingHTTPServer(("127.0.0.1", 8181), H); threading.Thread(target=srv.serve_forever, daemon=True).start()
from bridge import frames
import latency
latency.frames = lambda host: frames(host, 8181)
out = []
latency.watch("127.0.0.1", 1.2, out)
print("pictures", len(out), "brightness", [round(v) for _, v in out][:12])
from depth_tape import centre_value
from ne_budget import depth_coreml
from ultralytics.utils import ASSETS
print("depth centre value", round(centre_value(depth_coreml(), (ASSETS / "bus.jpg").read_bytes()), 3))
srv.shutdown()
EOF
```
Expected: `pictures 2x brightness [0, 0, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0]` and `depth centre value 0.41` (a `BrokenPipeError` from the stand-in server when the reader stops is harmless).

- [ ] **Step 7: Commit**

```bash
git add tools/measure/bridge.py tools/measure/latency.py tools/measure/depth_tape.py tools/measure/td_stats.py tools/measure/snap.py
git commit -m "feat(measure): tank measurement scripts - picture age, depth vs tape, gyro, ultrasonic, snapshots

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: On the tank (owner present)

**Files:** none (results recorded in Task 5).

Ask **"stand or floor?"**: the tank can be on the floor or the stand — **nothing drives in this task**; say so before each step. Only one phone may watch the camera meanwhile (the stream allows two viewers: one phone + the measurement script). Tank address: `192.168.31.167` (`--host` otherwise).

- [ ] **Step 1: Camera height, tilt and blind zone** — measure the camera lens height above the floor with a tape and note the tilt servo at its centre. Run `uv run python snap.py`; slide a tape mark along the floor toward the tank until it leaves the bottom of the picture: the distance from the front of the tracks is the **blind zone** (QVGA and VGA). Save a picture of an empty floor with marks at 50 cm and 100 cm (for Plan Q1's calibration).

- [ ] **Step 2: Picture age and rate** — point the camera at the laptop screen (~30 cm), close other camera viewers but one, run `uv run python latency.py --sizes QVGA,VGA --seconds 20`.
Expected: per size, pictures per second and the median / min / max age on arrival (spec 7 assumed 0.1-0.3 s, 10-20 pictures/s).

- [ ] **Step 3: Depth against the tape** — on the wood floor: `uv run python depth_tape.py --floor wood`; on the rug: `--floor rug` (a box ≥ 20 cm straight ahead at 0.3 / 0.6 / 1.0 / 2.0 m).
Expected: two tables per floor; the fit with the smaller errors tells whether the model's output is inverse depth; note the ultrasonic column.

- [ ] **Step 4: Gyro drift at rest (5 minutes)** — tank still, nobody touching it: `cd tools/smoke && uv run python td_record.py --host 192.168.31.167 --hz 50 --seconds 300 --out ../measure/out/rest.csv`, then `cd ../measure && uv run python td_stats.py gyro out/rest.csv`.

- [ ] **Step 5: Ultrasonic on surfaces (10 s each)** — the tank facing, at ~50 cm: a hard wall, the wall at about 45°, the sofa, a curtain, a chair leg, a soft toy. For each: `td_record.py --host 192.168.31.167 --hz 50 --seconds 10 --out ../measure/out/sonar-<name>.csv`, then `td_stats.py sonar out/sonar-<name>.csv`.
Expected: per surface the no-echo %, median and spread (research: fabric and slanted walls often give no echo).

---

### Task 5: Record the numbers

**Files:**
- Modify: `docs/design-notes.md`, `docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md` (§7, only if a target changes), `CLAUDE.md`

- [ ] **Step 1: design-notes** — add a section "Plan Q0 — phase 0 measurements (<date>)" (the next free section number) with one line per measurement: the Neural Engine budget (Task 2), camera height / tilt / blind zone, picture age and rate per size, depth errors per floor and which fit won, gyro drift per minute and the rate at rest, ultrasonic no-echo per surface; then the engine parameters they set: `camera.size`, `depth.every_n`, `gate_dps` (keep 45 unless the picture age says otherwise), the sonar beam half-angle (keep 12° unless the chair-leg test says otherwise), and whether the spec 7 §7 targets stand.
- [ ] **Step 2: Spec 7 §7** — only if a target has to change: edit the row and say why (with the measured number).
- [ ] **Step 3: CLAUDE.md** — add Plan Q0 to the plan list and the plans row ("phase 0 measurements — done <date>").
- [ ] **Step 4: Delete the pictures** — `rm -rf tools/measure/out` (they show the home; the numbers are recorded).
- [ ] **Step 5: Commit**

```bash
git add docs/design-notes.md docs/superpowers/specs/2026-10-20-07-ai-module1-map-design.md CLAUDE.md
git commit -m "docs: Plan Q0 phase 0 measurements recorded

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

