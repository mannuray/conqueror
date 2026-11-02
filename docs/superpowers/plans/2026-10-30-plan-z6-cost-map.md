# Plan Z6 — the cost map Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drive on the room map with one cost per cell, one per-point body keep-out and a standard recovery sequence, replacing Plan Z5's per-case rules (design-notes §164-§166).

**Architecture:** `CostMap` turns a `MapSnapshot` into lethal / near / unknown / free costs; `Footprint` checks the body's outline along any move (one keep-out rule) and reads its highest cost; the fan (`Planner`) and `RoutePlanner` both use those costs; `Recovery` runs look → turn → back → `blocked`; `RoamSkill` and `ChaseSkill` use all four. `RoomMap.body_over` makes the floor under the tank known free.

**Tech Stack:** Python 3.12 (uv), numpy, OpenCV (`cv2.distanceTransform`), pytest, ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §18.13 (approved 2026-10-30, design-notes §166.1); spec 9 (`docs/superpowers/specs/2026-10-27-09-explore-without-bumping-design.md`) for safety; spec 0 §1.1 principle 3.

## Global Constraints

- Branch `plan-z6` (from `plan-z5`); merge to `main` together with `plan-z5`, only after Task 9's floor checks and the owner's OK.
- Python: type hints everywhere, ruff + pyright strict clean; commit only when the whole suite passes (`ai-engine`: `uv run ruff format`, `uv run ruff check .`, `uv run pyright`, `uv run pytest -q`).
- Commits: Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Values (LLD §18.13): `KEEP_OUT_M` 0.075; `NEAR_OUT_M` 0.20; `NEAR_C_M` 0.40; `NEAR_COST` 0.8; `UNKNOWN_COST` 0.3; a cell's cost = min(0.95, near + unknown), lethal 1; `COST_W` 4; `ROUTE_COST_W` 6; `STUCK_S` 3.0; `BACK_M` 0.20; body front 0.13 / rear 0.09 / half-width 0.115 (`Limits`); body shrink for `body_over` 0.02; `body_over` at most 10 Hz, one step per cell per update.
- Speed from cost: top = 25 − (25 − 18) × c, never under `v_min` 18; 18 cm/s cap when the angle isn't trusted or no depth (unchanged).
- Never weaken safety: the sonar stop (`clear_m` 0.10), stale stop, STOP, deadman, the Uno's 15 cm brake unchanged; backing up only at `v_min` and only over known-free cells.
- The simulator's bar stays: the body never nearer than 5 mm to any box (`BODY_M`). A case failing only on a box never seen is reported to the owner, never passed by lowering the bar.
- No per-case rules: any new condition needs the owner's OK first (spec 0 §1.1 principle 3).
- `config/map.yaml` gets a `cost:` section; more careful only: `keep_out_m`, `near_out_m`, `near_c_m`, `near_cost`, `unknown_cost` only raised; `back_m` only lowered.

## Review Focus

1. **Put down close to something** (a mark already inside the keep-out at the start): it must still be able to move away, never toward it — Task 3 `test_already_near_may_move_away_not_nearer`.
2. **Pose drift and `body_over`:** a real mark 3 cm outside the body must survive the tank standing beside it for a minute — Task 2 `test_marks_just_outside_the_body_survive`.
3. **Nothing known behind** (the tank put down, no driving yet): recovery never backs up and ends `blocked` — Task 6 `test_never_backs_over_unknown_floor`.
4. **The chase's target in its own marks** (a ball by a sofa): still reached on the cost map — Task 7 sim `test_a_ball_by_a_sofa_is_reached` (kept, must pass).
5. **Tick time:** a full tick (cost map + route + fan) on the 160 × 160 map stays under 30 ms on the M1 — Task 7 Step "measure", printed, not a test.

---

### Task 1: `CostMap` and the `cost:` settings

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/costmap.py`
- Modify: `ai-engine/src/conqueror_ai/domain/depth.py` (`MapConfig.cost`), `ai-engine/src/conqueror_ai/adapters/map_config.py` (load + more-careful check), `ai-engine/config/map.yaml`
- Test: `ai-engine/tests/test_drive_costmap.py`, `ai-engine/tests/test_map_config.py`

**Interfaces:**
- Produces: `CostSettings` (frozen dataclass: `keep_out_m=0.075, near_out_m=0.20, near_c_m=0.40, near_cost=0.8, unknown_cost=0.3, cost_w=4.0, route_cost_w=6.0, stuck_s=3.0, back_m=0.20`); `MapConfig.cost: CostSettings`.
- Produces: `CostMap(snap: MapSnapshot, s: CostSettings = CostSettings(), ignore: tuple[float, float, float] | None = None)` with `n`, `cell_m`, `half_m`, `lethal: BoolArray`, `known_free: BoolArray` (= seen and not lethal), `dist: FloatArray` (metres from each cell centre to the nearest lethal cell centre, the map's border lethal), `settings`; methods `cell_of(x, y) -> tuple[int, int] | None`, `centre(ix, iy) -> XY`, `dist_m(x, y) -> float` (0 outside), `dist_many(xy: FloatArray) -> FloatArray` (0 outside), `seen_at(x, y) -> bool`, `outline_cost(xy: FloatArray, half_w: float) -> FloatArray` (the fan's reading: `near_cost × clip((near_out − d)/(near_out − keep_out), 0, 1)` + `unknown_cost` if unseen, capped 0.95; 1 where lethal or outside), `centre_cost(half_w: float) -> FloatArray` (per cell, for routes: `near_cost × clip((near_c − d)/(near_c − half_w), 0, 1)` + unknown, capped 0.95; 1 where lethal).
- `ignore=(x, y, r)`: lethal cells within r of (x, y) count as free (the chase's ball), as `ClearanceMap` does today.

- [ ] **Step 1: Write the failing tests** (`test_drive_costmap.py`, maps drawn with `RoomMap.hit` as in `test_drive_clearance.py`)
  - `test_lethal_is_marks_and_the_border`: a mark → `lethal` at its cell, `dist_m` 0 there; `dist_m(3.0, 0)` ≈ 1.0 on an empty seen map (border); outside → 0.
  - `test_outline_cost_by_distance`: seen map, a mark at (1.0, 0): `outline_cost` at d = 0.20 → 0; at d = 0.075 → 0.8; at d = 0.1375 → ≈ 0.4; in the mark → 1.
  - `test_unknown_adds_and_caps`: unseen open floor → 0.3; unseen at the keep-out → 0.95 (capped).
  - `test_centre_cost_for_routes`: d = 0.40 → 0; d = `half_w` (0.115) → 0.8; d = 0.2575 → ≈ 0.4.
  - `test_known_free_is_seen_and_not_lethal`; `test_ignore_disk` (as `ClearanceMap`'s).
  - `test_nothing_grows_into_unknown`: a seen mark beside unseen floor: the unseen cell 10 cm behind the mark is not lethal (cost 0.3 + near), `dist_m` there 0.10.
- [ ] **Step 2:** `cd ai-engine && uv run pytest tests/test_drive_costmap.py -q` → FAIL (no module `costmap`).
- [ ] **Step 3:** Write `costmap.py`; `dist` with `cv2.distanceTransform(..., DIST_MASK_PRECISE)` on a bordered free mask (as `clearance._dist`).
- [ ] **Step 4:** Add `cost:` to `MapConfig` / `load_map_config` / `config/map.yaml` (values above); refuse less careful values: test `test_cost_may_only_be_more_careful` in `test_map_config.py` (`keep_out_m: 0.05` and `back_m: 0.3` each → `ValueError` "more careful").
- [ ] **Step 5:** `uv run pytest tests/test_drive_costmap.py tests/test_map_config.py -q` → PASS; whole suite green.
- [ ] **Step 6:** Commit `feat(ai): the cost map - lethal, near, unknown, free (Plan Z6)`.

### Task 2: the body as a floor sensor (`RoomMap.body_over`)

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/room_map.py`, `ai-engine/src/conqueror_ai/app/map.py` (`MapRun`)
- Test: `ai-engine/tests/test_room_map.py`, `ai-engine/tests/test_map_app.py`

**Interfaces:**
- Produces: `RoomMap.body_over(pose: Pose, front_m: float, rear_m: float, half_w_m: float, t_us: int) -> bool` — the cells under the rectangle shrunk by `BODY_SHRINK_M` (0.02) get one miss on low and high and `seen` = true; at most once per 100 ms (`BODY_EVERY_US` 100 000); returns whether it applied.
- Produces: `MapRun(cal, cfg, depth, p=None, lim: Limits | None = None)`; `on_telem` calls `body_over` with `lim`'s body (default `Limits()`).

- [ ] **Step 1: Failing tests**
  - `test_the_floor_under_the_body_is_seen_and_free`: a low mark under the tank's centre (hit twice), `body_over` 3 times 100 ms apart → not blocked, seen.
  - `test_body_over_at_most_10_hz`: two calls 50 ms apart → the second returns False and changes nothing.
  - `test_marks_just_outside_the_body_survive` (Review Focus 2): a mark 3 cm beyond the side (y = 0.115 + 0.03) hit 4 times; 600 `body_over` calls over 60 s → still blocked.
  - `test_map_run_clears_under_the_tank` (`test_map_app.py`): replayed telemetry standing still → the cells under the pose seen.
- [ ] **Step 2:** run → FAIL (`body_over` missing).
- [ ] **Step 3:** implement (points every 2.5 cm inside the shrunk rectangle → `cells()`; one `miss` per cell per call).
- [ ] **Step 4:** run → PASS; suite green.
- [ ] **Step 5:** Commit `feat(ai): the floor under the tank is known free (Plan Z6)`.

### Task 3: `Footprint` — one body rule

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/footprint.py` (moves `_outline`, `_dense` out of `planner.py`)
- Test: `ai-engine/tests/test_drive_footprint.py`

**Interfaces:**
- Consumes: `CostMap` (Task 1), `Limits`.
- Produces: `Footprint(lim: Limits)` with `outline: FloatArray` (tank frame, a point every 2.5 cm), `world(p: XYTh) -> FloatArray`, `dense(at, poses) -> list[XYTh]` (≤ 5° / 2.5 cm apart), `allowed(cost: CostMap, at: XYTh, poses: list[XYTh]) -> bool` — per outline point i, along every dense pose: `d_i ≥ min(keep_out_m, d_i(at))`; `highest_cost(cost, at, poses, ahead_m: float | None = None) -> float` — the largest `outline_cost` over the dense poses (only those within `ahead_m` of `at` when given); `spin_poses(at, left: bool, deg: float = 90.0) -> list[XYTh]` (5° steps); `back_poses(at, length_m: float) -> list[XYTh]` (2.5 cm steps).

- [ ] **Step 1: Failing tests** (drawn maps, all seen unless stated)
  - `test_into_the_keep_out_is_refused_beside_it_is_dear`: a wall 30 cm ahead; a straight 1.5 s roll-out → refused; a pose ending 10 cm off → allowed, `highest_cost` > 0.5.
  - `test_run_2_beside_the_bed`: the panel (cells from y = −0.175 to −0.40, x −0.6..0.10) and leg (x 0.075..0.30, y −0.15..−0.40): left turn, right turn, straight → refused; `back_poses(at, 0.2)` → allowed.
  - `test_run_3_unknown_round_the_tank`: unseen everywhere within 0.3 m but one seen 2-cell mark at (0.42, 0.15): both `spin_poses` allowed; `highest_cost` ≈ 0.3 + near.
  - `test_a_turn_is_checked_its_own_way`: a box behind the right rear corner (x −0.30..−0.125, y −0.25..−0.15): left allowed, right refused.
  - `test_already_near_may_move_away_not_nearer` (Review Focus 1): a wall 3 cm beyond the left side: a turn right (away) allowed; a curve left refused.
- [ ] **Step 2:** `uv run pytest tests/test_drive_footprint.py -q` → FAIL.
- [ ] **Step 3:** implement with `CostMap.dist_many` / `outline_cost` (vectorised per pose).
- [ ] **Step 4:** PASS; suite green (planner still uses its own copies until Task 4).
- [ ] **Step 5:** Commit `feat(ai): one body rule - the outline keeps 7.5 cm, or no nearer (Plan Z6)`.

### Task 4: the fan on the cost map

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/planner.py`; the callers in `roam.py` / `chase.py` switch the fan's map from `ClearanceMap(snap)` to `CostMap(snap, …)` (mechanical: routes keep their `ClearanceMap` until Task 5)
- Test: `ai-engine/tests/test_drive_planner.py` (the map tests rewritten to costs)

**Interfaces:**
- Consumes: `CostMap`, `Footprint`.
- Produces: `Planner(lim, weights=None)` gains `foot: Footprint`; `choose(at, now, target, obstacle_m=None, cost: CostMap | None = None, far_m=None) -> Command`; `score(..., cost: CostMap | None = None, ...)`: forbidden (`foot.allowed` false, spins via `spin_poses` the way they turn) → `+ w.blocked`; otherwise `− cost.settings.cost_w × foot.highest_cost(cost, at, path)`; `can_spin(at, cost, left, deg=90.0) -> bool`; `speed_cap(cost, at, cmd) -> float` = 25 − 7 × `highest_cost` over the first 0.5 m of `cmd`'s roll-out (≥ `v_min`); `choose` returns the best move with `v` capped to `speed_cap` and `w` scaled by the same factor. Removed: `MAP_CLEAR_M`, `FRONT_CLEAR_M`, `BODY_CLEAR_M`, `SPIN_MARGIN_M`, `body_ok`, `_inside`, `_least`.

- [ ] **Step 1: Failing tests** (`test_drive_planner.py`)
  - keep (rewritten to `cost=`): `test_the_fan_keeps_off_marked_obstacles`, `test_all_ways_blocked_stops`, `test_an_empty_map_changes_nothing`, `test_a_spin_from_driving_coasts_and_is_checked_too`, `test_the_tanks_front_is_kept_clear_not_only_its_centre`, `test_a_wider_body_keeps_its_sides_clear`, `test_beside_the_bed_it_turns_away_not_into_it` (with the drawn panel 17.5 cm off: left allowed, right refused).
  - new `test_speed_from_cost`: open seen floor → 25; all unseen → 25 − 7 × 0.3 = 22.9 (± 0.1); a wall 12 cm beside the path → between 18 and 25; never under 18.
  - new `test_cost_steers_away_from_things`: a wall 15 cm left of a straight path to a target ahead → the chosen curve bends right (w < 0).
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** implement; delete the removed constants and methods.
- [ ] **Step 4:** PASS; whole suite green. A simulated roam that now fails is a finding: debug it (the fan is the only change), never relax the test.
- [ ] **Step 5:** Commit `feat(ai): the fan scores moves by cost; speed from cost (Plan Z6)`.

### Task 5: routes on the cost map

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/route.py`, `ai-engine/src/conqueror_ai/domain/drive/goals.py`
- Test: `ai-engine/tests/test_drive_route.py`, `ai-engine/tests/test_drive_goals.py`

**Interfaces:**
- Consumes: `CostMap.centre_cost`, `CostMap.dist`.
- Produces: `RoutePlanner(lim: Limits)`; `route(cost: CostMap, start, goal, goal_ok=False, near_m=0.0) -> list[XY] | None`; `costs(cost, start) -> FloatArray`; a 10 cm cell is closed when its centre is nearer than `lim.body_half_w_m` to a lethal cell (2 × 2 worst), step cost = length × (1 + `route_cost_w` × the cell's `centre_cost`); the start cell always open. `WanderGoal.destination(now, at, cost)`, `ExploreGoal.destination(now, at, cost, routes)` (frontier: seen, open, next to unknown). Removed: `ROBOT_R_M`, `NEAR_W`, `NEAR_FROM_M`, `NEAR_SPAN_M`, `UNSEEN_W`, `ESCAPE_M`, `TIGHT_W`, `TURN_MARGIN_M`, `turn_room_m`, `clearance.py` (when nothing imports it).

- [ ] **Step 1: Failing tests**
  - kept, on `CostMap`: straight in the open, around a box, out of a U, none, unseen costs more, start cell allowed, target in its own marks (`ignore`).
  - `test_doorways_stay_open`: walls with a 46 cm gap (cell-aligned as the sim's) → a route.
  - `test_room_to_turn_comes_from_cost`: a ~40 cm gap straight ahead and a ~60 cm gap 80 cm left → the route goes through the wide one; the narrow alone → a route.
  - goals: wander picks seen spots with `dist_m` ≥ 0.30; explore's frontier reachable; explore done when all seen.
- [ ] **Step 2:** run → FAIL. **Step 3:** implement; the skills pass their `CostMap` to routes and goals; delete `clearance.py` and its tests when nothing imports it. **Step 4:** PASS, suite green. **Step 5:** Commit `feat(ai): routes on the same costs (Plan Z6)`.

### Task 6: `Recovery` — look, turn, back, blocked

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/recovery.py`
- Test: `ai-engine/tests/test_drive_recovery.py`

**Interfaces:**
- Consumes: `Footprint`, `CostMap`, `Limits`, `Command` / `STOP` (`curves.py`). It does not import `chase.py` (which imports it): the camera tilt comes in as `Recovery(..., tilt_deg)`.
- Produces: `Recovery(lim: Limits, foot: Footprint, tilt_deg: float)`; `start(now_s, at)` — begins at the next step if `at` is within `PROGRESS_M` (0.3) of the last start, else at "look"; `tick(now_s, at, cost, dt_s) -> tuple[Command, tuple[float, float] | None] | None` — the step's command and camera (pan, tilt) or None; `None` when a step has finished (the skill then tries to drive); `step: Literal["look", "turn", "back", "blocked"]`; `progress(at)` — resets when `at` is ≥ 0.3 m from the last start. Steps: **look** pans `(-90, -45, 45, 90)` then 0, standing 1 s each (`LOOK_STAND_S`); **turn** 90° on the spot the allowed way, the cheaper (`highest_cost`) first, re-checked each tick; **back** straight back at `−v_min`, ≤ `back_m`, ≤ 2.5 s, only while every outline cell of the next poses is `known_free` and `allowed`; **blocked** → the skill ends `blocked`.

- [ ] **Step 1: Failing tests**
  - `test_the_order_look_turn_back_blocked`: a skill-less driver calling `start` / `tick` at the same spot with nothing allowed → steps seen in that order, then `blocked`.
  - `test_look_pans_and_stands`: the camera commands `-90, -45, 45, 90, 0`, the tank STOP throughout.
  - `test_turn_the_allowed_way`: only right allowed → `w < 0`.
  - `test_never_backs_over_unknown_floor` (Review Focus 3): unknown behind → no `v < 0`, goes to `blocked`.
  - `test_backs_over_known_free_floor_20_cm_at_v_min`: commands `Command(-18, 0)`, a simulated tank ends 0.18-0.22 m back.
  - `test_progress_resets_the_sequence`: after moving 0.3 m, `start` begins at look again.
- [ ] **Step 2:** FAIL. **Step 3:** implement. **Step 4:** PASS, suite green. **Step 5:** Commit `feat(ai): the standard ways out - look, turn, back, blocked (Plan Z6)`.

### Task 7: the skills on the cost map

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/roam.py`, `chase.py`, `ai-engine/src/conqueror_ai/app/drive.py` (cost settings and `Limits` into the skills and `MapRun`)
- Test: `ai-engine/tests/test_drive_roam.py`, `test_drive_roam_sim.py`, `test_drive_chase.py`, `test_drive_app.py`

**Interfaces:**
- Consumes: everything above.
- Produces: `RoamSkill(planner, mode, seconds=None, find=False, rng=None, cost: CostSettings | None = None)`, states `start`, `drive`, `recover`, `held`, `chase`, `done`, `failed`; one `CostMap` per snapshot; stuck = `choose` returns STOP or no progress ≥ 0.05 m for `stuck_s`, or no route → `Recovery.start`; `Recovery` `None` → back to `drive`; `blocked` → `_end("blocked")`. `ChaseSkill(planner, cost: CostSettings | None = None)`: on a map, routes on `CostMap(ignore=ball)` and recovery as roam; without a map, slice 1 unchanged. Removed from roam: the look's spin steps, `LOOK_PANS` there, `STUCK_*`, `LOOK_ROUNDS`, `track`, `_start_back` / `_back` / `_back_ok`, `BACK_*`, `TRACK_*`.
- `RoamSkill.start` look: stand 1 s camera ahead (unchanged).

- [ ] **Step 1: Failing tests**
  - roam unit: start look then drive; stuck → recovery steps in order → `blocked`; wander time; explore done; echo < 10 cm `blocked`; stale STOP / 2 s `stale`; slow when untrusted; find → chase; cancel.
  - sim (`test_drive_roam_sim.py`, `body_over` called each tick in the harness; bar `BODY_M` 0.005 unchanged): all existing tests, plus `test_run_2_layout`, `test_run_3_layout` (a 10 × 5 cm box 40 cm ahead-left; also 35 cm ahead, 45 cm ahead-right), `test_wall_beside_at_the_start` (the two `test_no_turn_on_the_spot_scrapes_a_wall` cases) — none ends `blocked` while a way exists; explore ≥ 80 %.
  - chase: `test_chase_goes_around_a_box`, `test_a_ball_by_a_sofa_is_reached` (Review Focus 4), slice-1 tests without a map unchanged.
- [ ] **Step 2:** FAIL. **Step 3:** implement; delete the removed code. **Step 4:** PASS; whole suite green.
- [ ] **Step 5: measure** (Review Focus 5): a scratch timing of 100 ticks of `RoamSkill.tick` on the replayed run-1 map → print the median ms; over 30 ms → profile and note in the ledger before going on.
- [ ] **Step 6:** Commit `feat(ai): wander, explore, find and the chase on the cost map (Plan Z6)`.

### Task 8: replay check on the floor-run recordings

**Files:** none committed (scratch script); recordings stay local and are deleted after Task 9.

- [ ] **Step 1:** Replay `recordings/z5-w`, `z5-e`, `z5-e2` through `MapRun` with `body_over`, and at run 2's (20 s) and run 3's (12 s) poses ask the real `Planner` / `Footprint` / `RoutePlanner` what they'd do; along run 1's path, the speed cap.
- [ ] **Expected:** run 3 → a turn on the spot; run 2 → refused everywhere but backing over its driven cells; run 1 → median ≥ 24 cm/s. Different → systematic debugging before Task 9.
- [ ] **Step 2:** Write the numbers into the ledger (they go into design-notes in Task 9).

### Task 9: floor checks (owner present, STOP in hand)

Before each run: say what moves (the tank drives itself at ≤ 25 cm/s, turns on the spot, the camera pans, it may back up ≤ 20 cm over floor it drove on), confirm floor, numbered steps, `--record`.

- [ ] **Step 1:** `cq-drive wander 120 --record recordings/z6-w` — no touches; goes round more than one corner of the room.
- [ ] **Step 2:** `cq-drive explore --record recordings/z6-e` — `done`, ≥ 80 % of the room seen (by the owner's eye and `cq-map` replay).
- [ ] **Step 3:** `cq-drive goto ball --record recordings/z6-g` with a box between — `done`, no touch.
- [ ] **Step 4:** `cq-drive find ball --record recordings/z6-f` with the ball out of sight — `done`.
- [ ] **Step 5:** replay with `cq-map` for the numbers; delete the recordings (they show the home); design-notes §167 (numbers and how each was derived); commit `docs: Plan Z6 floor checks (design-notes §167)`.

### Task 10: as built

- [ ] **Step 1:** AI engine LLD §18.14 "Z6 as built": files, the class diagram and the RoamSkill state diagram as built, every deviation from §18.13 with its reason; mark §18.6-§18.7's rules and §164-§165's as replaced.
- [ ] **Step 2:** CLAUDE.md: Plan Z6 in the plans list and the roadmap line.
- [ ] **Step 3:** Commit `docs: Plan Z6 as built (AI engine LLD §18.14, CLAUDE.md)`; push; ask the owner to merge `plan-z5` and `plan-z6`; then give the worktree and branch removal commands.
