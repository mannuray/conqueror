# Plan Z5 — driving on the map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The tank drives about a room on its own without bumping into anything: `cq-drive wander [seconds]`, `cq-drive explore`, `cq-drive find <thing>`, and `cq-drive goto <thing>` now going around a box.

**Architecture:** Each tick the room map's snapshot (Plan Z4) becomes a `ClearanceMap` (metres to the nearest obstacle per cell); a goal picks a destination; `RoutePlanner` (A* on a 10 cm grid) finds a route around obstacles; slice 1's fan of curves aims at a point 0.5 m along it, each curve checked against the clearance. `RoamSkill` runs wander / explore / find and hands over to slice 1's `ChaseSkill`, which now aims along routes too. Everything is tested first on a simulated room (`SimRoom`) on the laptop.

**Tech Stack:** Python 3.12 (uv), numpy, OpenCV (`cv2.distanceTransform`, `cv2.connectedComponents`, already there), heapq, pytest, ruff, pyright strict.

**Spec:** `docs/superpowers/specs/2026-10-27-09-explore-without-bumping-design.md` (§4 driving, §5 safety, §6 pass marks); design: `docs/lld/ai-engine-lld.md` §18.6-§18.8 (slice 1: §17; the map as built: §18.12).

## Global Constraints
- **Unchanged from slice 1:** the 25 cm/s cap; turning on the spot beyond 35°; the stale map stops the tank at once and ends the run after 2 s; STOP from the phone; Ctrl-C; the Uno's 15 cm brake and the bridge's deadman underneath.
- **The sonar has the last word:** an echo under `clear_m` that isn't the target ends the run `blocked`, whatever the map says (LLD §18.8). **`clear_m` is 0.10 m from the tank's front** (owner, 2026-10-29: 30 cm ended runs too early; it was 0.30 in slice 1); the Uno's own 15 cm brake is unchanged and usually acts first. Task 3 changes `Limits.clear_m` and `config/drive.yaml` to 0.10.
- **The map never invents free space:** unseen cells may be routed through only at extra cost; the fan's clearance treats unseen as free only because the camera sees them before the tank arrives (spec 9 §4.2).
- **Slower when unsure:** **18 cm/s** while the camera angle isn't trusted or there was no depth picture in the last second (spec 9 §5.3-5.4).
- **Depth fails:** marks from the sonar only, 18 cm/s, **`explore` refuses to start** with a clear message.
- **Routes:** a 10 cm grid (each cell the worst of its 2 × 2 map cells), 8-connected; **not allowed:** clearance < `ROBOT_R_M` 0.15 m; step cost = length × (1 + `NEAR_W` 3 · max(0, 0.35 − clearance) / 0.20 + `UNSEEN_W` 1 if unseen); the start cell always allowed; ≤ 20 ms per route in Python on 80 × 80 (else re-plan every second tick).
- **The fan on the map:** a curve whose rolled-out poses come within `ROBOT_R_M − 0.03` (0.12 m) of an obstacle, and nearer than where the tank is now, scores `blocked` (−10).
- **Aim:** 0.5 m along the route.
- **Wander:** a destination 1.5-2.0 m away, seen-free with clearance ≥ 0.30 m, scored `cos(bearing) + 0.5·openness + noise(0.3)`; a new one when within 0.4 m, every 6 s, or with no route; ends after the given seconds (none: when stopped).
- **Explore:** frontier = seen-free cells next to unseen ones (10 cm grid), groups under 3 cells (30 cm) ignored; the cheapest reachable one; none → `done`.
- **No route:** a look (360° on the spot in 90° steps, standing ~1 s after each so the map takes pictures); still none → wander picks another destination, explore ends `done`, the chase ends `blocked`.
- **Never toward a person** (slice 1: `goto` / `find` never take `person`).
- Floor runs: the owner present, STOP in hand, the steps said before each drive. Recordings deleted after the numbers are written.
- Python: type hints, ruff + pyright strict clean, no blocking the event loop; Conventional commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; checks chained with `&&` and `set -o pipefail` before any commit.

## Review Focus
1. **The tank already closer than 0.12 m to something** (stopped by a box, or the map just marked a cell under it): curves that move away must still be allowed, or it stands forever — test in Task 3.
2. **The chase's target sits in its own marks** (the ball is 12 cm tall: low and high marks around it): the route must still reach it — test in Task 2 (the target's own disk is ignored).
3. **Start or destination outside the map, or the start cell blocked:** no exception; the start cell is allowed, a destination outside is "no route" — test in Task 2.
4. **A map that changes under a route** (a new box appears on the route): the next tick's route goes around it, the fan never drives into it — test in Task 6 (simulated).
5. **Every frontier unreachable, or the room fully seen:** explore ends `done`, never spins forever — test in Task 5 / 6.

---

### Task 1: `ClearanceMap`

**Files:** Create `ai-engine/src/conqueror_ai/domain/drive/clearance.py`; Test `ai-engine/tests/test_drive_clearance.py`.

**Interfaces:**
- Consumes: `MapSnapshot` (Z4: `blocked`, `seen`, `cell_m`, arrays `[ix, iy]`, origin at the map's centre).
- Produces: `ClearanceMap(snap: MapSnapshot, ignore: tuple[float, float, float] | None = None)` — `ignore` = (x, y, r): blocked cells within r of (x, y) count as free (the chase's target); `dist: FloatArray (n, n)` metres from each cell centre to the nearest blocked cell or the map's edge; `dist_m(x: float, y: float) -> float` (0.0 outside the map); `seen_at(x, y) -> bool`; `n`, `cell_m`, `half_m`; `cell_of(x, y) -> tuple[int, int] | None`; `centre(ix, iy) -> tuple[float, float]`.

- [ ] **Step 1: Failing tests:** `test_empty_map_clearance_is_distance_to_edge` (at (0, 0) of an empty 8 m map ≈ 4.0 m ± 0.05); `test_a_blocked_cell` (one blocked cell at (1, 0): `dist_m(1.5, 0) ≈ 0.5 ± 0.05`, `dist_m(1, 0) == 0`); `test_outside_is_zero`; `test_ignore_disk` (cells blocked around (1, 0) within 0.1 m, `ignore=(1, 0, 0.12)` → `dist_m(1, 0)` > 3); `test_unseen_counts_free` (an all-unseen map has the same `dist` as all-seen).
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_drive_clearance.py -q` → FAIL (module missing).
- [ ] **Step 3:** Implement with `cv2.distanceTransform` (`DIST_L2`, mask 5) on the free mask padded by one blocked cell all round (the edge is a wall), × `cell_m`.
- [ ] **Step 4:** same → PASS; then `uv run pytest -q && uv run ruff check . && uv run pyright` clean.
- [ ] **Step 5:** Commit `feat(ai): ClearanceMap - metres to the nearest obstacle (Plan Z5)`.

### Task 2: `RoutePlanner` and `aim_point`

**Files:** Create `ai-engine/src/conqueror_ai/domain/drive/route.py`; Test `ai-engine/tests/test_drive_route.py`.

**Interfaces:**
- Consumes: `ClearanceMap` (Task 1).
- Produces: constants `ROBOT_R_M = 0.15`, `NEAR_W = 3.0`, `UNSEEN_W = 1.0`, `GRID_M = 0.10`; `XY = tuple[float, float]`; `RoutePlanner()` with `route(clear: ClearanceMap, start: XY, goal: XY, goal_ok: bool = False) -> list[XY] | None` (room points, start first, goal last; `goal_ok`: the goal's cell allowed whatever its clearance) and `costs(clear: ClearanceMap, start: XY) -> FloatArray` (Dijkstra over the 10 cm grid: cost to every cell, `inf` where unreachable; for explore); `grid_centre(i, j) -> XY`; `aim_point(route: list[XY], at: XY, ahead_m: float = 0.5) -> XY`.

- [ ] **Step 1: Failing tests:** `test_straight_route_in_the_open` (start (0, 0) → goal (2, 0): ends at the goal, length ≤ 2.1 m); `test_around_a_box` (a 40 × 40 cm box blocked at x 0.8-1.2, y −0.2-0.2 → a route to (2, 0) whose every point has `dist_m` ≥ 0.15); `test_out_of_a_u` (a U open toward −x around the start → a route out to (2, 0) exists); `test_no_route` (the goal walled in → None); `test_unseen_costs_more` (two equal-length ways, one through unseen cells → the seen one); `test_start_cell_allowed` (start 0.08 m from a wall → a route exists); `test_target_in_its_own_marks` (Review Focus 2: a ball's marks within 0.1 m of (1.5, 0), `ClearanceMap(snap, ignore=(1.5, 0, 0.12))`, `goal_ok=True` → a route); `test_goal_outside_map_is_none` (Review Focus 3); `test_aim_point` (a straight route: the point 0.5 m on from the nearest route point; the last point when shorter); `test_costs_inf_where_walled`; `test_route_is_quick` (an 8 m map with 30 random boxes, 20 routes across it: the slowest < 0.02 s — the budget; if it fails the fix is in the code, not the test).
- [ ] **Step 2:** `uv run pytest tests/test_drive_route.py -q` → FAIL.
- [ ] **Step 3:** Implement A* with `heapq` on the 10 cm grid (each grid cell's clearance = min of its 2 × 2, seen = any of them), 8-connected, octile heuristic; the route's grid cells turned into room points, the exact start and goal at the ends.
- [ ] **Step 4:** → PASS; full suite, ruff, pyright clean.
- [ ] **Step 5:** Commit `feat(ai): RoutePlanner - A* on the room map around obstacles (Plan Z5)`.

### Task 3: The fan on the map

**Files:** Modify `ai-engine/src/conqueror_ai/domain/drive/planner.py`; Test `ai-engine/tests/test_drive_planner.py`.

**Interfaces:**
- Consumes: `ClearanceMap`, `ROBOT_R_M`.
- Produces: `Planner.score(..., clear: ClearanceMap | None = None)` and `Planner.choose(at, now, target, obstacle_m=None, clear: ClearanceMap | None = None)`; with `clear`, a curve with a rolled-out pose whose `dist_m` < 0.12 **and** < the start's `dist_m` − 0.01 scores `blocked`. Without `clear` nothing changes (slice 1's tests stay green).

- [ ] **Step 1: Failing tests:** `test_the_fan_keeps_off_marked_obstacles` (a box marked 0.6 m straight ahead, target 2 m beyond it → the chosen curve is not straight on); `test_already_close_may_move_away` (Review Focus 1: the tank 0.08 m from a wall on its left, target ahead → `choose` is not STOP); `test_all_ways_blocked_stops` (boxed in on every side within 0.2 m, target ahead → STOP); `test_without_a_map_as_before` (same choice as slice 1 for a few targets).
- [ ] **Step 2:** `uv run pytest tests/test_drive_planner.py -q` → the new tests FAIL.
- [ ] **Step 3:** Implement as above; `Limits.clear_m` 0.30 → 0.10 and `config/drive.yaml` to match (the drive config's "more careful only" check follows the new default); slice 1's tests that used 0.30 updated to the new value.
- [ ] **Step 4:** → PASS; full suite clean.
- [ ] **Step 5:** Commit `feat(ai): the fan of curves checks the room map (Plan Z5)`.

### Task 4: `SimRoom` — a room for simulated drives

**Files:** Modify `ai-engine/src/conqueror_ai/domain/drive/sim.py`; Test `ai-engine/tests/test_drive_sim.py`.

**Interfaces:**
- Consumes: `RoomMap`, `Marks`, `MapSettings`, `Pose`.
- Produces: `Box(x0, y0, x1, y1, z0=0.0, z1=0.15)` (an overhang has z0 > 0); `SimRoom(boxes: list[Box], walls_m: float = 3.0)` (square walls ±walls_m); `marks(at: XYTh, half_deg=41.0, near_m=0.30, far_m=2.0) -> Marks` (rays every 1° across the view, points every 5 cm: floor until the first box, then low / high points by the box's z-range; an overhang (z0 ≥ 0.02) gives high points and floor under it); `sonar(at) -> float | None` (the nearest box edge or wall straight ahead of the sonar within ±12°, boxes with z0 < 0.10 only, ≤ 4 m); `gap_m(at) -> float` (tank centre to the nearest box or wall edge); `front_inside(at) -> bool` (the point 0.13 m ahead of the centre inside a box or wall); `reachable_floor(room: RoomMap) -> float` and `seen_share(room) -> float` (seen share of the floor cells the tank's centre could reach, i.e. ≥ 0.15 m from every box and wall).

- [ ] **Step 1: Failing tests:** `test_simroom_sees_a_box_low` (a box 1 m ahead → low points near x 1.0, floor points before it, none behind); `test_simroom_overhang_is_high_with_floor_under` (Box z0 0.15, z1 0.20 → high points and floor points under it); `test_simroom_sonar` (box 1 m ahead → ≈ 0.87 m from the sonar; an overhang with z0 0.15 ahead and the wall 3 m on → the wall's distance: the beam passes under); `test_gap_and_front`.
- [ ] **Step 2:** → FAIL. **Step 3:** implement. **Step 4:** → PASS; suite clean.
- [ ] **Step 5:** Commit `test(ai): SimRoom - a simulated room with boxes and an overhang (Plan Z5)`.

### Task 5: The goals

**Files:** Create `ai-engine/src/conqueror_ai/domain/drive/goals.py`; Test `ai-engine/tests/test_drive_goals.py`.

**Interfaces:**
- Consumes: `ClearanceMap`, `RoutePlanner.costs`, `bearing_deg`.
- Produces:
  - `WanderGoal(seconds: float | None, rng: random.Random)`: `destination(now_s, at, clear) -> XY | None` (keeps the current one until within 0.4 m, 6 s old, or `drop()` was called); `drop()`; `done(now_s) -> bool` (`seconds` passed since the first call).
  - `ExploreGoal()`: `destination(now_s, at, clear, planner) -> XY | None` (None = nothing reachable left → done); frontiers as in Global Constraints; a frontier reached (within 0.4 m) or found unreachable is remembered (30 cm cells) and not picked again; re-picked every 3 s; `done: bool`.

- [ ] **Step 1: Failing tests:** `test_wander_picks_open_floor_ahead` (an open seen map: the destination 1.5-2.0 m away, `dist_m` ≥ 0.30, mostly ahead over 50 seeds: mean |bearing| < 60°); `test_wander_new_when_near_or_old_or_dropped`; `test_wander_done_after_seconds` and `test_wander_without_seconds_never_done`; `test_explore_goes_to_the_nearest_frontier` (seen in front, unseen beyond 1.5 m → a destination on that edge); `test_explore_ignores_tiny_unseen_bits` (a 20 cm unseen hole inside seen floor → not picked); `test_explore_done_when_nothing_left` (Review Focus 5: all seen → None, `done`); `test_explore_done_when_unreachable` (frontier behind a wall → None).
- [ ] **Step 2:** → FAIL. **Step 3:** implement (frontiers with `cv2.connectedComponents`, 8-connected; one `costs()` per pick). **Step 4:** → PASS; suite clean.
- [ ] **Step 5:** Commit `feat(ai): wander and explore pick where to go on the map (Plan Z5)`.

### Task 6: `RoamSkill`

**Files:** Create `ai-engine/src/conqueror_ai/domain/drive/roam.py`; Test `ai-engine/tests/test_drive_roam.py` (unit) and `ai-engine/tests/test_drive_roam_sim.py` (whole simulated runs).

**Interfaces:**
- Consumes: Tasks 1-5, `ChaseSkill`, `SkillOut`, `Limits`, `ramp`, `Goal`/`TargetFix` (slice 1), `MapSnapshot`.
- Produces: `RoamSkill(planner: Planner, mode: Literal["wander", "explore"], seconds: float | None = None, find: bool = False, rng: random.Random | None = None)` with `state: str` (`look`, `drive`, `held`, `chase`, `done`, `failed`), `tick(now_s, at: XYTh | None, snap: MapSnapshot | None, fix: TargetFix | None, sonar_m: float | None, map_ok: bool, cam_still: bool = True) -> SkillOut` and `cancel() -> SkillOut`; results `done`, `blocked`, `stale`, `cancelled`, and for find, the chase's results. The state diagram is LLD §18.7's. Speed cap 18 cm/s (a `Limits` with `v_cap_cms=18`) while `snap.angle_trusted` or `snap.depth_ok` is false. The camera: straight ahead at the driving tilt (−15°), panned toward the aim point in 8° steps within ±60° (as the chase).

- [ ] **Step 1: Failing unit tests:** `test_starts_with_a_look_then_drives`; `test_no_route_looks_round_in_90_deg_steps`; `test_wander_new_destination_after_a_look_without_route`; `test_explore_done_when_goal_says_so`; `test_echo_under_10_cm_ends_blocked` (an echo at 0.12 m does not end it, at 0.09 m does); `test_stale_stops_at_once_and_ends_after_2_s`; `test_slow_when_angle_untrusted_or_no_depth` (max v 18 cm/s); `test_find_hands_over_to_the_chase` (a fix arrives → state `chase`, the next commands come from the chase); `test_cancel_stops`.
- [ ] **Step 2: Failing simulated runs** (`SimRoom` with three boxes and an overhang; the loop as `test_drive_sim.chase` does: `marks(at)` into a `RoomMap` every 0.1 s when not turning faster than 45°/s, `sonar(at)`, `SimTank.step`): `test_wander_60_s_keeps_moving_never_close` (60 s: the centre never within 0.12 m of a box or wall, the front never inside one, the result `done` at 60 s, and the tank covered > 3 m of path); `test_explore_covers_80_percent` (`seen_share` ≥ 0.8 at `done`, never closer than 0.12 m); `test_a_box_appears_on_the_route` (Review Focus 4: a box added mid-run in front → it goes around, never closer than 0.12 m); `test_find_ball_out_of_sight` (a ball behind a box, not in view at the start → found, chased, `done`).
- [ ] **Step 3:** → FAIL (module missing). **Step 4:** implement. **Step 5:** → PASS; suite clean.
- [ ] **Step 6:** Commit `feat(ai): RoamSkill - wander, explore and find on the room map (Plan Z5)`.

### Task 7: The chase goes around

**Files:** Modify `ai-engine/src/conqueror_ai/domain/drive/chase.py`; Test `ai-engine/tests/test_drive_chase.py`, `ai-engine/tests/test_drive_roam_sim.py`.

**Interfaces:**
- Produces: `ChaseSkill.tick(..., snap: MapSnapshot | None = None)`: with a snapshot, the approach builds `ClearanceMap(snap, ignore=(ball, 0.12))`, routes to the ball (`goal_ok=True`), aims the fan at `aim_point(route, at)` with `clear`; arrival, the arrival turn and `echo_is_target` still use the ball; no route → slice 1's rule (STOP, `blocked` after 3 s). Without a snapshot, slice 1 unchanged. The 18 cm/s rule as in Task 6.

- [ ] **Step 1: Failing tests:** `test_chase_goes_around_a_box` (simulated: a 30 cm box between the tank and the ball 2 m ahead → `done`, never closer than 0.12 m to the box); `test_chase_without_a_map_unchanged` (slice 1's sim tests still pass — they pass no snapshot).
- [ ] **Step 2:** → FAIL. **Step 3:** implement. **Step 4:** → PASS; suite clean.
- [ ] **Step 5:** Commit `feat(ai): the chase drives around obstacles on the map (Plan Z5)`.

### Task 8: `cq-drive wander | explore | find | goto` on the map

**Files:** Modify `ai-engine/src/conqueror_ai/app/drive.py`, `ai-engine/src/conqueror_ai/app/map.py`; Test `ai-engine/tests/test_drive_app.py`, `ai-engine/tests/test_map_app.py`.

**Interfaces:**
- Consumes: `MapRun` (Z4), the skills.
- Produces:
  - `MapRun(cal, cfg, depth, p: ObjectPipeline | None = None)` (cq-drive hands in its pipeline with the detectors); `MapRun.gate(f, rgb, dets: list[Detection] | None = None)`.
  - `run_drive(host, action: Literal["goto", "wander", "explore", "find"], thing: str | None, seconds: float | None, rec, use_depth) -> str` replacing `goto` (the loop of slice 1, with the full `Seeing` (not angle only) and the map: depth and seeing off the loop, `update_picture` and the sonar on it); the snapshot's `stale` worked out when the snapshot is taken (Z4 deferred minor); a crashed link or camera task ends the run with its error (as now).
  - The CLI: `cq-drive goto <thing>`, `cq-drive find <thing>`, `cq-drive wander [seconds]`, `cq-drive explore`; `explore` without depth → `SystemExit("explore needs the depth model (models/DepthAnythingV2SmallF16.mlpackage): without it the map has only the distance sensor")`; `find` / `goto` words as slice 1 (`goal_words`, never `person`); a status line per tick (state, speed, turn rate, map seen m²).
- [ ] **Step 1: Failing tests:** `test_cli_words` (argument parsing: `wander 60`, `wander`, `explore`, `find ball`, `goto ball`; `find person` refused); `test_explore_refuses_without_depth`; `test_mapgate_passes_detections`; `test_drive_run_never_moves_without_a_fresh_map` (fake bridge with telemetry but no pictures → the result `stale - ...` or `no pictures`, and only STOP / no `drive_rate` with speed > 0 was sent).
- [ ] **Step 2:** → FAIL. **Step 3:** implement. **Step 4:** → PASS; suite clean; `uv run cq-drive --help` lists the four actions.
- [ ] **Step 5:** Commit `feat(ai): cq-drive wander, explore and find on the room map (Plan Z5)`.

### Task 9: Floor checks (owner present; the tank drives itself — STOP in hand)

No code unless a check fails (then: systematic debugging, a failing test, a fix, a ledger ruling). Before each run: what moves (the tank drives on its own at ≤ 25 cm/s, the camera pans), floor confirmed, numbered steps; `--record` on every run; a box ~10 cm, the bed edge, a chair in the room.

- [ ] **Step 1: Wander 2 minutes** (`cq-drive wander 120 --record recordings/z5-w`). **Pass (spec 9 §6.3):** no touches (the owner watches; a firing of the Uno brake is logged as a near miss), smooth driving; it ends `done` at 120 s (or `blocked` by the sonar rule — counted and discussed).
- [ ] **Step 2: Explore** (`cq-drive explore`). **Pass (§6.4):** it stops by itself `done`; ≥ 80 % of the reachable floor seen (replay: the map's seen floor against the floor the owner says is reachable, by eye on the map picture).
- [ ] **Step 3: The chase with a box in the path** (`cq-drive goto ball`, the ball ~2 m ahead behind a box). **Pass (§6.5):** `done`, around the box.
- [ ] **Step 4: Find the ball out of sight** (`cq-drive find ball`, the ball behind the tank or behind furniture). **Pass (§6.6):** `done`.
- [ ] **Step 5:** Replay each recording with `cq-map` for the map numbers; delete the recordings; design-notes (next free number) with how each number was derived; commit `docs: Plan Z5 floor checks (design-notes §N)`.

### Task 10: Docs as built
- [ ] **Step 1:** AI engine LLD §18.13 "Z5 as built": files, changes from §18.6-§18.7 with reasons, the floor numbers, Mermaid class and state diagrams as built.
- [ ] **Step 2:** `CLAUDE.md`: the plan in the list, the order line (slice 2 done; next per the owner: Plan Z2 the Tank Logo words, the fence, option C, nudging).
- [ ] **Step 3:** Commit and push; merge only when the owner says so.
