# Plan Z4 — the room map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One room-sized map, fixed where the tank started, that fills in from the depth picture's height marks and the distance sensor while the owner drives with the phone, shown live in `cq-map`.

**Architecture:** `RoomMap` (pure numpy, 160 × 160 cells of 5 cm) takes `Marks` from Plan Z3's `Seeing` once per picture and `SonarModel` echoes from telemetry; `snapshot()` hands out copies with the stale flag for Plan Z5. `cq-map` runs the map pipeline only (no detector, never drives): a replay of a recording, or live from the tank with an optional recording, with the map as an image in the Rerun view.

**Tech Stack:** Python 3.12 (uv), numpy, OpenCV (already there), Rerun (already there), pytest, ruff, pyright strict.

**Spec:** `docs/superpowers/specs/2026-10-27-09-explore-without-bumping-design.md` (spec 9 §3 items 5-8, §5); design: `docs/lld/ai-engine-lld.md` §18.5, §18.8, §18.9 (Z3 as built: §18.11).

## Global Constraints
- The map: **8 × 8 m, 5 cm cells (160 × 160), fixed where the tank started** (the start at the centre, heading 0 along +x); room frame x forward, y left (LLD §13).
- Each cell: log-odds `low`, `high` (float32), `seen` (bool). **Hit +0.85, miss −0.4, clamped to ±4; blocked when `low > 0.7` or `high > 0.7`** (LLD §18.5 / §18.9).
- **Fading only in view:** a cell is lowered only by a floor point seen in it in this picture; nothing else lowers a mark from the camera.
- **The sonar:** sonar point = pose + `sonar_x_m` (0.13 m) ahead; an echo `d` < `sonar_max_m` (4.0 m): a **low hit** on the arc at `d` (±12°), a **low miss** in the cone (±4°) from 0.10 m to `d − 0.10`; **never touches `high`**; no echo or `d ≥ 4.0` does nothing; at most 10 samples a second used.
- **The map's edge counts as blocked** (a soft fence); points and echoes outside the map are ignored.
- **Stale** (spec 7 §4, as `cq-drive`): no picture for 0.5 s or no telemetry for 0.2 s. `depth_ok`: a depth picture in the last 1 s.
- `cq-map` **never sends anything that moves the tank** (only `BridgeLink`, which never drives, and the camera stream).
- `config/map.yaml` new values may only be made more careful: `blocked` and `hit` not lower, `miss` not more negative than the defaults (the "more careful only" rule, LLD §17.10 / §18.9).
- Recordings show the home: kept in the git-ignored `ai-engine/recordings/`, deleted after the numbers are written down.
- Python: type hints everywhere, ruff + pyright strict clean, no blocking the event loop; commits Conventional, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; run checks with `set -o pipefail`.

## Review Focus
1. **One picture puts thousands of points in a near cell:** that cell moves **one step** per picture, not thousands (else one picture saturates the map and one bad picture is permanent) — test in Task 2.
2. **A cell with both floor points and obstacle points in the same picture** (a box's foot): it gets the hit and **no miss** — the map never invents free space — test in Task 2.
3. **Points, echoes or the tank beyond the 8 × 8 m map:** ignored without an error; the edge reads blocked — tests in Tasks 2 and 3.
4. **No depth model, or a picture with no floor in view (`marks` None):** the map still runs from the sonar, `depth_ok` goes false after 1 s — tests in Tasks 2 and 4.
5. **An echo nearer than the cone's start (e.g. 5 cm):** the arc hit only, no cone, no negative-length loop — test in Task 3.

---

### Task 1: The map's settings

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/depth.py` (settings dataclasses)
- Modify: `ai-engine/src/conqueror_ai/adapters/map_config.py`
- Modify: `ai-engine/config/map.yaml`
- Test: `ai-engine/tests/test_map_config.py`

**Interfaces:**
- Produces: `MapSettings(size_m: float = 8.0, cell_m: float = 0.05, hit: float = 0.85, miss: float = -0.4, clamp: float = 4.0, blocked: float = 0.7)` (frozen dataclass in `domain/depth.py`); `Ranges.sonar_max_m: float = 4.0`; `MapConfig.map: MapSettings` (default `MapSettings()`); `load_map_config` reads a `map:` section.

- [ ] **Step 1: Write the failing tests** in `tests/test_map_config.py`:
  - `test_map_section_defaults`: `load_map_config(missing path).map == MapSettings()` and `.ranges.sonar_max_m == 4.0`.
  - `test_map_section_read`: a file with `map: {hit: 0.9}` gives `cfg.map.hit == 0.9`, the rest default.
  - `test_map_less_careful_refused`: each of `{blocked: 0.5}`, `{hit: 0.5}`, `{miss: -0.6}` raises `ValueError` naming the key.
  - `test_map_size_must_hold_whole_cells`: `{size_m: 8.0, cell_m: 0.03}` raises `ValueError` (8.0 / 0.03 is not a whole number); `{cell_m: 0}` raises.
  - `test_repo_map_yaml_loads`: `load_map_config(MAP_CONFIG).map == MapSettings()`.
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_map_config.py -q` — Expected: FAIL (`MapSettings` not defined).
- [ ] **Step 3: Implement.** `MapSettings` and `Ranges.sonar_max_m` in `depth.py`; `MapConfig.map`; in `map_config.py` accept the section `map`, refuse `blocked < 0.7`, `hit < 0.85`, `miss < −0.4`, `cell_m ≤ 0`, `size_m ≤ 0`, and `round(size_m / cell_m)` not within 1e-9 of `size_m / cell_m`. Add to `config/map.yaml`: `sonar_max_m: 4.0` under `ranges` and a `map:` section with the six defaults, each with a short comment (LLD §18.5).
- [ ] **Step 4:** same command — Expected: PASS; then the whole suite `uv run pytest -q` passes.
- [ ] **Step 5: Commit** `feat(ai): the room map's settings in config/map.yaml (Plan Z4)`.

### Task 2: `RoomMap` and `MapSnapshot`

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/room_map.py`
- Test: `ai-engine/tests/test_room_map.py`

**Interfaces:**
- Consumes: `Marks` (Z3: `floor_xy`, `low_xy`, `high_xy`, `above_xy`, each `(n, 2)` room metres), `MapSettings`, `Pose`.
- Produces:
  - `RoomMap(settings: MapSettings = MapSettings())` with `n: int` (160), `cell_m`, arrays `low`, `high` (float32 `(n, n)`), `seen` (bool `(n, n)`); index `[ix, iy]` with `ix = floor((x + size_m/2) / cell_m)`, the same for y.
  - `cells(xy: FloatArray) -> NDArray[np.intp]` — unique flat cell indices inside the map (outside dropped).
  - `hit(cells: NDArray[np.intp], layer: Literal["low", "high"]) -> None`, `miss(cells, layer) -> None` — one step each, clamped.
  - `update_picture(marks: Marks, t_us: int) -> None`.
  - `blocked() -> NDArray[np.bool_]`; `is_blocked(x: float, y: float) -> bool` (True outside the map).
  - `snapshot(t_us: int, pose: Pose | None, stale: bool, angle_trusted: bool) -> MapSnapshot`.
  - `MapSnapshot` (frozen): `t_us, pose, blocked, high, seen, stale, depth_ok, angle_trusted` (arrays are copies; `high` = high-layer blocked); `depth_ok` = an `update_picture` within 1 s before `t_us`.
  - `map_image(snap: MapSnapshot) -> NDArray[np.uint8]` — `(n, n, 3)` RGB, image up = +x, image right = −y (row `n−1−ix`, column `n−1−iy`): unseen black (0,0,0), seen grey (110,110,110), blocked low red (220,40,40), blocked high orange (255,150,0) (high wins), the tank's cell and its 8 neighbours white when `pose` is given.

- [ ] **Step 1: Write the failing tests** in `tests/test_room_map.py` (Marks built by hand):
  - `test_start_is_the_centre`: `RoomMap().cells(np.array([[0.0, 0.0]]))` is the cell `(80, 80)`; `[[3.99, -3.99]]` → `(159, 0)`; `[[4.01, 0]]` → empty.
  - `test_floor_point_misses_and_sees`: one floor point at (1, 0) → that cell `low == high == −0.4`, `seen`; every other cell untouched.
  - `test_low_and_high_hits`: low at (1, 0) and high at (1, 0.5) → `low[c] == 0.85`, `high[c2] == 0.85`, both `seen`; `blocked()` true at both after one picture (0.85 > 0.7).
  - `test_above_only_sees`: an above point → `seen` true, `low` and `high` 0.
  - `test_one_step_per_cell_per_picture` (Review Focus 1): 5 000 low points in one cell → `low == 0.85` after one picture, `1.7` after two.
  - `test_a_hit_beats_a_miss_in_the_same_picture` (Review Focus 2): the same cell with 1 000 floor points and 1 low point → `low == 0.85`, `high == 0.0`.
  - `test_clamped`: 10 pictures of hits → `low == 4.0`; then 20 of floor → `−4.0`.
  - `test_fades_only_where_seen`: a blocked cell with no points for 50 pictures (other cells get floor points) stays blocked; floor points on it twice (0.85 − 0.8 = 0.05) → not blocked.
  - `test_outside_ignored_edge_blocked` (Review Focus 3): marks at (10, 0) change nothing and don't raise; `is_blocked(4.5, 0)` and `is_blocked(0, -4.5)` are True; `is_blocked(0, 0)` False.
  - `test_snapshot_copies_and_flags` (Review Focus 4): after `update_picture(m, 1_000_000)`, `snapshot(1_500_000, pose, False, True).depth_ok` True, `snapshot(2_100_000, …).depth_ok` False; a fresh map's snapshot has `depth_ok` False; changing the map afterwards doesn't change the snapshot's arrays; `stale` and `angle_trusted` are passed through.
  - `test_map_image_colours`: a low-blocked cell red, a high-blocked one orange, a seen one grey, an unseen one black, the pose's cell white, at the rows / columns above.
  - `test_update_picture_is_quick`: 20 000 floor + 2 000 low + 2 000 high + 2 000 above random points inside the map: the fastest of 5 `update_picture` calls < 0.01 s (it runs on the event loop, Task 5).
- [ ] **Step 2:** `uv run pytest tests/test_room_map.py -q` — Expected: FAIL (module missing).
- [ ] **Step 3: Implement** `room_map.py`. `update_picture`: unique cells per class (`np.unique` of flat indices); `hit` low cells and high cells; floor cells minus every hit cell (low or high) this picture get `miss` on both layers; `seen` set for every class's cells; store `t_us` as the last depth picture. A docstring with LLD §18.5.
- [ ] **Step 4:** same command — Expected: PASS; `uv run pytest -q`, `uv run ruff check .`, `uv run pyright` clean.
- [ ] **Step 5: Commit** `feat(ai): RoomMap - one room-sized map of low / high marks and seen (Plan Z4)`.

### Task 3: `SonarModel`

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/sonar_model.py`
- Test: `ai-engine/tests/test_sonar_model.py`

**Interfaces:**
- Consumes: `RoomMap.cells`, `RoomMap.hit`, `RoomMap.miss` (Task 2), `Pose`.
- Produces: `SonarModel(sonar_x_m: float = 0.13, half_deg: float = 12.0, cone_deg: float = 4.0, near_m: float = 0.10, max_m: float = 4.0, every_us: int = 100_000)`; `update(room: RoomMap, pose: Pose, dist_m: float | None, t_us: int) -> bool` (True when the sample was used).

- [ ] **Step 1: Write the failing tests** in `tests/test_sonar_model.py` (pose at the start, heading 0, unless said):
  - `test_echo_marks_the_arc_low`: `dist_m` 1.0 → `room.low` > 0 at (1.13, 0), at (1.13·cos 10°, 1.13·sin 10°) roughly (the cell of `sonar point + 1.0 m` at +10°), and 0 at +20°; `room.high` all 0.
  - `test_cone_cleared_low_only`: first a low hit and a high hit put by hand in the cell of (0.63, 0); an echo at 1.0 m → that cell's `low` = 0.85 − 0.4, its `high` still 0.85 (the sonar never touches high).
  - `test_no_echo_changes_nothing`: `None`, `4.0` and `5.0` → `update` returns False and `low`, `high`, `seen` are unchanged.
  - `test_ten_per_second`: echoes of 1.0 m at t 0, 50 000, 100 000 µs → used True, False, True; a no-echo sample does not use up the slot (None at 0, then 1.0 m at 50 000 → True).
  - `test_turns_with_the_pose`: pose heading 90° at (1, 1) → the arc hit is at (1, 1 + 0.13 + 1.0).
  - `test_very_near_echo` (Review Focus 5): `dist_m` 0.05 → an arc hit near (0.18, 0), no miss anywhere (no cell below 0).
  - `test_beyond_the_map` (Review Focus 3): pose at (3.9, 0), echo 1.0 m → no error; cells inside the map on the cone are lowered, nothing outside.
  - `test_arc_has_no_gaps`: echo at 3.5 m → every map cell crossed by the ±12° arc (checked by sampling the arc every 1 mm) has `low > 0`.
  - `test_does_not_mark_seen`: after an echo, `room.seen` is all False (only the camera turns unseen into seen, spec 9 §5).
- [ ] **Step 2:** `uv run pytest tests/test_sonar_model.py -q` — Expected: FAIL (module missing).
- [ ] **Step 3: Implement.** Points on the arc every `cell_m / 2` of arc length, points in the cone every `cell_m / 2` along and across; arc cells hit low; cone cells minus arc cells missed low; skip the cone when `d − 0.10 ≤ near_m`.
- [ ] **Step 4:** same command — Expected: PASS; whole suite, ruff, pyright clean.
- [ ] **Step 5: Commit** `feat(ai): SonarModel - an echo marks low, the cone clears low only (Plan Z4)`.

### Task 4: `cq-map` replay and the map in the live view

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/map.py`
- Modify: `ai-engine/src/conqueror_ai/adapters/rerun_view.py`, `ai-engine/pyproject.toml` (`cq-map = "conqueror_ai.app.map:main"`)
- Test: `ai-engine/tests/test_map_app.py`

**Interfaces:**
- Consumes: Z3's `ObjectPipeline` (detections always `[]`), `Seeing`, `load_depth`, `depth_due`, `decode`, `see` (from `app/objects.py`); `RoomMap`, `SonarModel`, `map_image`.
- Produces:
  - `MapRun(cal: Calibration, cfg: MapConfig, depth: DepthFn | None)` — `p: ObjectPipeline`, `room: RoomMap`, `sonar: SonarModel`, `seeing: Seeing`; `on_calib(counts_per_m: int)`; `on_telem(s: TelemSample)` (`p.on_telem`, then `sonar.update(room, pose, dist_m, s.bus_us)` when the pose exists; `dist_mm` None or ≤ 0 is no echo); `on_text(text: str)` (a recorded message: `calib` → `on_calib`, `td` → `on_telem`, as `run_replay` in `app/objects.py` does; Task 5's live loop calls `on_calib` / `on_telem` directly); `on_picture(f: Frame, rgb) -> Seen | None` (gate, depth when due, `see`, `room.update_picture(seen.marks, f.cap_us)` when marks exist); counters `used`, `depth_pictures`, `marked`, `echoes`.
  - `run_replay(folder: Path, cal, cfg, depth) -> MapRun`; `report(run: MapRun) -> str` with the lines `pictures used N, with depth N, with marks N; echoes used N` and `map: seen X.X m², blocked low N cells, high N cells`.
  - `RerunView.room_map(t_us: int, snap: MapSnapshot) -> None` — logs `map/image` (`map_image`) and `map/stale` as a scalar 0/1.
  - `cq-map <folder>` (replay, prints the report; `--no-view` leaves the viewer out, otherwise it shows the map after each picture).

- [ ] **Step 1: Write the failing tests** in `tests/test_map_app.py`, with a recording written by `SessionRecorder` in `tmp_path` and a fake depth function for a flat floor (`1 / expected_floor_depth()` of the camera at the recording's tilt, 0 where there is no floor), both as the depth replay tests in `test_objects_app.py` build them:
  - `test_replay_marks_floor_seen`: the report says `with marks` > 0 and `seen` > 0.5 m²; no blocked cells.
  - `test_replay_echo_marks_low`: the same recording with `dist_mm` 800 in every `td` → `blocked low` > 0, `high` 0.
  - `test_replay_without_depth` (Review Focus 4): `depth=None` → `with depth 0`, `seen 0.0 m²`, the echoes still mark low.
  - `test_replay_same_answer_twice`: two replays give equal `room.low`, `room.high`, `room.seen`.
  - `test_no_echo_values_ignored`: `dist_mm` 0 and missing → `echoes used 0`.
- [ ] **Step 2:** `uv run pytest tests/test_map_app.py -q` — Expected: FAIL (module missing).
- [ ] **Step 3: Implement** `app/map.py` (`MapRun`, `run_replay`, `report`, `main` with `folder`, `--live`, `--host`, `--record`, `--no-depth`, `--no-view`; `--live` raises `SystemExit("--live comes in Task 5")` until Task 5) and `RerunView.room_map`; module docstring with two usage lines, like `cq-objects`.
- [ ] **Step 4:** same command — Expected: PASS; whole suite, ruff, pyright clean; `uv run cq-map --help` prints the usage.
- [ ] **Step 5: Commit** `feat(ai): cq-map replays a recording into the room map (Plan Z4)`.

### Task 5: `cq-map --live`, recording and the stale flag

**Files:**
- Modify: `ai-engine/src/conqueror_ai/app/map.py`
- Modify: `ai-engine/tests/fake_bridge.py` (test helper: real pictures, pictures that stop)
- Test: `ai-engine/tests/test_map_app.py`

**Interfaces:**
- Consumes: `BridgeLink`, `MjpegSource`, `SessionRecorder`, `see_off_loop`, `map_state` (`app/drive.py`, the stale rule), `MapRun` (Task 4).
- Produces: `async run_live(link: BridgeLink, cam: MjpegSource, run: MapRun, rec: SessionRecorder | None, view: RerunView | None, seconds: float | None = None, stop: asyncio.Event | None = None) -> MapRun` (link and stream handed in, as `app/record.py`'s `record` does, so tests aim it at the fake bridge); once a second it prints `map: N/s, seen X m², blocked low N high N` plus the stale reason when stale; every picture's snapshot goes to the view when there is one. `main --live` builds the link and stream from `--host`.
- Test helper: `FakeBridge(..., jpeg: bytes = JPEG, frames_for_s: float | None = None)` — the picture it streams (tests pass a real 640 × 480 JPEG made with `cv2.imencode`), and stop streaming pictures after that many seconds (the socket and telemetry carry on).

- [ ] **Step 1: Write the failing tests** with `tests/fake_bridge.py` (as `test_record.py` uses it: `BridgeLink("127.0.0.1", bridge.ws_port)`, `MjpegSource("127.0.0.1", bridge.stream_port)`, `seconds=` to end the run) and Task 4's flat-floor fake depth:
  - `test_live_fills_the_map`: a fake bridge sending `td` and pictures for 1 s → the returned run has `used` > 0 and `seen` cells.
  - `test_live_records`: with a `SessionRecorder` → the folder replays (`run_replay`) to the same `seen` count ± the last picture.
  - `test_live_never_drives`: the fake bridge saw no `drive_rate`, `move`, `turn` or `cam_set` message.
  - `test_live_stale_reported`: `FakeBridge(frames_for_s=0.3)`, run 2 s → a printed line (capsys) contains `stale` and `no new picture`.
- [ ] **Step 2:** `uv run pytest tests/test_map_app.py -q` — Expected: the new tests FAIL.
- [ ] **Step 3: Implement** `run_live`: the telemetry task feeds `MapRun.on_text`-equivalent per sample (`p.on_telem`, the sonar); the picture loop: depth in a worker thread when due, `see_off_loop`, `room.update_picture` on the loop (Task 2 keeps it < 10 ms), `snapshot` with `stale = map_state(...) is not None`; Ctrl-C ends it quietly and closes the links (as `cq-drive`).
- [ ] **Step 4:** same command — Expected: PASS; whole suite, ruff, pyright clean.
- [ ] **Step 5: Commit** `feat(ai): cq-map --live fills the map while the phone drives (Plan Z4)`.

### Task 6: Floor checks (owner present; the owner drives with the phone)

No code unless a check fails (then: systematic debugging, a test that fails first, a fix, a ledger ruling). **What moves:** the owner drives the tank with the phone; `cq-map` only watches. Before each run: the steps, numbered, and "floor" confirmed.

- [ ] **Step 1: Rate.** `uv run cq-map --live --record recordings/z4-a`, the tank standing 20 s with the camera at −15°. **Pass:** ≥ 10 map updates / s (spec 9 §3.8), the stale flag off.
- [ ] **Step 2: Fill the room.** The owner drives slowly about the room for ~2 minutes (a box ~10 cm, the bed edge in view at some point). Watch the map. **Pass:** the box's cells blocked low (red), the bed edge blocked high (orange) — spec 9 §6.2: no obstacle 10 cm or taller shown free within 1 m of where the tank passed; the floor driven over grey, not blocked.
- [ ] **Step 3: Fading and memory.** Drive past the box, turn away (box out of view) for 10 s, come back. **Pass:** the box stayed blocked while out of view; it stays blocked when seen again.
- [ ] **Step 4: Sonar never clears high.** Stop with the bed edge ~50 cm ahead (sonar echo under it or off it). **Pass:** the bed edge stays orange.
- [ ] **Step 5: Stale.** Cover the camera or stop the stream 2 s (owner): the line says stale, then recovers.
- [ ] **Step 6:** Replay the recording (`uv run cq-map recordings/z4-a --no-view`) twice: the same report both times; write down the numbers. Delete the recordings. Write design-notes (next free number) with how each number was derived; commit `docs: Plan Z4 floor checks (design-notes §N)`.

### Task 7: Docs as built
- [ ] **Step 1:** AI engine LLD §18.12 "Z4 as built": files, any change from §18.5 with the reason, the numbers from Task 6, a Mermaid class diagram of `RoomMap`, `SonarModel`, `MapSnapshot`, `MapRun`.
- [ ] **Step 2:** `CLAUDE.md`: Plan Z4 in the plans list and the order line (next: Plan Z5, driving on the map).
- [ ] **Step 3:** Commit and push; merge only when the owner says so.
