# Plan Z7 — the engine's structure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the AI engine's glue the domain's shape: one perception loop for every tool, a pure `World` handing out a `SkillInput`, one `Skill` interface with fixed results, a `Driver` port, cost maps per snapshot that carry the play area, and every drive run's decisions logged and replayable — with no behaviour change on the floor except the two LLD §19.8 names.

**Architecture:** Bottom-up: shared names and per-snapshot cost maps first (pure domain), then the skill interface on both skills, then `World` (pure, app) and `Perception` (async, app) replacing the three loops, then `SkillRunner` + `Driver` port under `cq-drive`, then the decisions log and `--replay`. Every task keeps the whole suite green.

**Tech Stack:** Python 3.12, uv, numpy, asyncio, pytest (+ pytest-asyncio as used today), ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §19 (the design), design-notes §191 (the owner's decisions), `docs/reviews/2026-11-02-ai-engine-architecture-review.md` (findings F1-F10 with file:line).

## Global Constraints

- Python 3.12 via uv; type hints everywhere; `uv run ruff check .`, `uv run ruff format --check .`, `uv run pyright` clean (strict) — **run unpiped**, each must exit 0 before a commit (§190: piping hid two failures).
- `domain/` never imports `app/` or `adapters/`, never reads a clock or prints.
- Safety never weakened: the stale stop (stop at once, end after 2 s), the echo rule (`clear_m` 0.10 → blocked), the footprint keep-out, the play area, never a person — each keeps its test.
- No floor behaviour change except (LLD §19.8): the chase's no-map path deleted; candidates beyond the nearest `ROUTE_TRIES` 3 wait a try.
- Existing tests keep their checks; a test may change only its call shape (`SkillInput`), never what it asserts — any other change needs a ledger ruling.
- Values: `Limits.slow_cms` 18.0; `Limits.drive_tilt_deg` -15.0; `SAME_SPOT_M` 0.5; `SURE_REACH_M` 1.0; `ROUTE_TRIES` 3; `TICK_S` 0.2.
- Recordings and decision logs show the home: local only, git-ignored, deleted after use.
- Commits: Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch `plan-z7` from main.

## Review Focus

1. A skill made anywhere (the chase inside find, a fresh chase after "lost") must stay inside `--area` — `CostMaps` carries it; test in Task 2.
2. 20+ pending candidates (memory from earlier runs, Plan Z8) must not stall a tick past the deadman's 0.5 s — at most 3 routes a try; test in Task 2.
3. `used` must be true for exactly one `input()` after a used picture, never for a skipped or repeated one (§190 I2) — test in Task 5.
4. A replayed log with a skill change must show the first differing tick, not crash or pass silently — test in Task 8.
5. A link failure, a refusal or STOP from the phone mid-run must end `cq-drive` with `FAILED` / `REFUSED` and the drive's finish (stop sent) — test in Task 7.

---

### Task 1: Shared names

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/limits.py`, `chase.py`, `roam.py`, `goals.py`, `candidates.py`, `domain/memory.py`, `domain/objects.py`, `adapters/drive_config.py`, `app/drive.py`
- Create: `ai-engine/src/conqueror_ai/domain/reach.py`
- Test: `ai-engine/tests/test_reach.py`

**Interfaces:**
- Produces: `Limits.slow_cms: float = 18.0`, `Limits.drive_tilt_deg: float = -15.0`; `domain/reach.py`: `SAME_SPOT_M = 0.5`, `SURE_REACH_M = 1.0`. `chase.DRIVE_TILT`, `roam.SLOW_CMS`, `chase.SLOW_CMS`, `drive_config.SLOW_CMS`, `memory.MERGE_M`, `candidates.MERGE_M`, `objects.SAME_THING_M`, `candidates.CLOSE_M`, `goals.LOOK_M` removed (call sites read the new names).

- [ ] **Step 1: Write the failing test** `test_reach.py`:
```python
def test_one_name_each() -> None:
    from conqueror_ai.domain import reach
    from conqueror_ai.domain.drive import candidates, chase, goals, roam
    from conqueror_ai.domain.drive.limits import Limits
    assert (reach.SAME_SPOT_M, reach.SURE_REACH_M) == (0.5, 1.0)
    assert (Limits().slow_cms, Limits().drive_tilt_deg) == (18.0, -15.0)
    for mod, name in ((chase, "SLOW_CMS"), (roam, "SLOW_CMS"), (chase, "DRIVE_TILT"),
                      (candidates, "MERGE_M"), (candidates, "CLOSE_M"), (goals, "LOOK_M")):
        assert not hasattr(mod, name), (mod.__name__, name)
```
- [ ] **Step 2:** `uv run pytest tests/test_reach.py -q` — Expected: FAIL (no module `reach`).
- [ ] **Step 3:** Add the names; replace every use (`grep -rn` the removed names in src and tests); the camera's driving tilt everywhere is `lim.drive_tilt_deg` (skills hold `self.lim`); `drive_config` checks `lim.v_min_cms > Limits().slow_cms`.
- [ ] **Step 4:** `uv run pytest -q` — Expected: all pass (576 + 1). Then ruff, format, pyright — each exit 0.
- [ ] **Step 5: Commit** `refactor(ai): one name each for the slow speed, the driving tilt, the same spot and the sure reach (LLD §19.2)`.

### Task 2: Cost maps per snapshot; candidates ranked

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/costmaps.py`
- Modify: `chase.py` (no `area` attribute; takes `CostMaps`), `roam.py` (no area hand-over; `_costmap` gone), `candidates.py` (`route`/`next` take `CostMaps`; ranking), `recovery.py` if it builds maps
- Test: `ai-engine/tests/test_drive_costmaps.py`, `tests/test_drive_candidates.py`

**Interfaces:**
- Produces: `class CostMaps: __init__(snap: MapSnapshot, settings: CostSettings | None, area: tuple[float, float, float] | None)`; `base() -> CostMap`; `ignoring(x: float, y: float, r: float) -> CostMap` (cached by `(round(x, 2), round(y, 2), r)`); `area` property. `Candidates.route(c, at, costs: CostMaps, routes) -> list[XY] | None`; `Candidates.next(at, costs: CostMaps, routes) -> Candidate | None` (area read from `costs.area`); `ROUTE_TRIES = 3`.
- Until Task 3 the skills build their `CostMaps` from `(snap, self.settings, self._area)` themselves (one per snapshot, cached by identity, as `_costmap` did).

- [ ] **Step 1: Write the failing tests:**
  - `test_one_build_per_snapshot_and_key` — `cm.base() is cm.base()`; `cm.ignoring(1.0, 0.0, 0.12) is cm.ignoring(1.0, 0.0, 0.12)`.
  - `test_the_area_is_always_applied` — with area `(0, 0, 1.0)`, `cm.base().lethal` and `cm.ignoring(...).lethal` are true at a cell 1.5 m out.
  - `test_a_chase_inside_find_never_leaves_the_area` (Review Focus 1, in `test_drive_roam_sim.py`): `roam("search", 60.0, find=True, ball=(2.0, 0.0), area_m=1.0)` (the find mode as it is before Task 4) → `r.out_m <= 1.0 + 0.05` and the result is not `done` at the out-of-area ball.
  - `test_twenty_pending_spots_cost_at_most_three_routes_a_try` (Review Focus 2): 20 candidates added; a counting `RoutePlanner` subclass; one `next()` → ≤ 3 `route` calls, and it returns the nearest reachable one.
- [ ] **Step 2:** run them — Expected: FAIL (no `costmaps`).
- [ ] **Step 3:** Implement; `ChaseSkill` gets the area only through the `CostMaps` it is handed; `Candidates.next` sorts pending by `math.dist(c.spot, at[:2])`, routes the first `ROUTE_TRIES`.
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `refactor(ai): cost maps per snapshot carry the play area; candidates ranked before routing (LLD §19.3)`.

### Task 3: The skill interface — types and the chase

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/drive/skill.py`
- Modify: `chase.py` (`tick(now_s, inp)`; no-map path deleted: `snap is None` branches, `BLOCKED_GIVE_UP_S`, `fan is None`), its tests (`tests/test_drive_chase.py`: call shape only)
- Test: `ai-engine/tests/test_drive_skill.py`

**Interfaces:**
- Produces (exactly as LLD §19.4): `SkillInput` (frozen: `at, snap, costs, fix, sonar_m, map_ok, cam_still, used, maybes`), `Result(StrEnum)` with `DONE="done", NOT_FOUND="not_found", LOST="lost", BLOCKED="blocked", STALE="stale", CANCELLED="cancelled", REFUSED="refused", FAILED="failed"`, `SkillOut(cmd, cam=None, result: Result | None = None, detail="")` (moved from chase.py), `class Skill(Protocol)`: `state: str`, `tick(now_s, inp) -> SkillOut`, `cancel() -> SkillOut`, `debug() -> dict[str, object]`. Helper `make_input(at, snap, area, settings, fix, sonar_m, map_ok, cam_still, used, maybes) -> SkillInput` (builds `CostMaps` when `snap` is set) — the one place inputs are assembled (World and the sims use it).
- `ChaseSkill.debug()` → `{"state", "target", "recovering"}`.

- [ ] **Step 1: Write the failing tests:** `test_results_are_their_old_strings` (`Result.NOT_FOUND == "not_found"` etc. — logs and the CLI read the same words); `test_make_input_builds_cost_maps_only_with_a_map`; `test_the_chase_is_a_skill` (`isinstance(ChaseSkill(...), Skill)` via `runtime_checkable`); `test_the_chase_has_no_map_free_path` (`not hasattr(chase, "BLOCKED_GIVE_UP_S")`; a tick with `snap=None` and `map_ok=True` stops and counts as stale, like roam).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; move every chase test to `SkillInput` (checks unchanged); tests of the deleted no-map path are deleted with a ledger ruling naming each (LLD §19.8).
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `refactor(ai): SkillInput, Result, the Skill protocol; the chase on it, its no-map path gone (LLD §19.4)`.

### Task 4: The skill interface — RoamSkill; memory's clock

**Files:**
- Modify: `roam.py` (`tick(now_s, inp)`; `mode: Literal["wander", "explore", "find"]`, no `find` flag; `DestinationGoal` chosen once), `goals.py` (the protocol), `domain/memory.py`, `app/drive.py` (`make_skill`, `_tick` → `skill.tick(now, make_input(...))`), roam tests and `tests/test_drive_roam_sim.py` (call shape: build inputs with `make_input`)
- Test: `tests/test_drive_roam.py`, `tests/test_memory.py`

**Interfaces:**
- Produces: `class DestinationGoal(Protocol)`: `done: bool`; `destination(now_s, at, cost, routes) -> XY | None`; `drop() -> None` — `WanderGoal`, `ExploreGoal`, `SearchGoal` satisfy it (`WanderGoal.done` always False). `RoamSkill.debug()` → `{"state", "dest", "route_len", "pending", "check_phase"}`. `ThingMemory.update(objects, wall_s: float, run_id: str)`; `Remembered(word, x_m, y_m, seen_wall_s, run_id, gone=False)`; `of_kind` orders by `seen_wall_s`.
- `make_skill(m, lim, cost, memory) -> Skill`; `Mission.action == "find"` → `RoamSkill(planner, "find", ...)`.

- [ ] **Step 1: Write the failing tests:** `test_find_is_a_mode_not_a_flag` (`RoamSkill(..., "find")` works; `"find" not in inspect.signature(RoamSkill).parameters`); `test_the_goals_are_one_interface` (each goal is a `DestinationGoal`); `test_roam_is_a_skill`; `test_memory_orders_by_wall_time_across_runs` (two runs' things: the newer wall time first, whatever the capture clock).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; move roam and sim tests to `make_input` (checks unchanged); the sim harness's per-tick input assembly becomes `make_input` (one shared place, LLD §19.10).
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `refactor(ai): RoamSkill on the Skill interface, find a mode, one goal interface; memory on wall time (LLD §19.4, §19.7)`.

### Task 5: World

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/world.py` (`MapRun` moved here from `app/map.py`; `World`)
- Modify: `app/map.py`, `app/drive.py`, `app/objects.py` imports; `tests/test_map_app.py` imports
- Test: `ai-engine/tests/test_world.py`

**Interfaces:**
- Consumes: `make_input` (Task 3), `CostMaps` (Task 2), `ThingMemory.update(objects, wall_s, run_id)` (Task 4).
- Produces: `class World: __init__(cal, cfg, depth, floor, sizes, lim, kind, *, room: RoomMap | None = None, memory: ThingMemory | None = None, area_m: float | None = None, run_id: str = "")`; `on_calib(counts_per_m: int)`; `on_telem(s: TelemSample)`; `on_picture(f: Frame, rgb, dets: list[Detection], cam: CameraModel | str, inv: FloatArray | None) -> CameraModel | str`; `input(now_s: float, wall_s: float, map_ok: bool) -> SkillInput`; properties `map_run`, `memory`, `sonar_m`. `on_picture` returns the gate's result and does what `drive_session` did inline (gate, `by_size`, seeing when `inv` given); `input` does the goal update, memory update, maybes, `used` (true once per used picture), the area set at the first pose.

- [ ] **Step 1: Write the failing tests:** `test_world_gives_the_maps_answers` (a tiny recording replayed through `World` and through today's `MapRun` path: same `seen` / `blocked` at the end, same objects); `test_used_is_true_once_per_used_picture` (Review Focus 3: picture used → `input().used` True, the next `input()` without a picture False; a picture skipped "turning" → False); `test_a_handed_in_room_and_memory_are_used` (`World(..., room=r, memory=m)`: `w.map_run.room is r`, `w.memory is m`); `test_the_area_starts_at_the_first_pose`.
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; `drive_session` uses `World` (its inline glue removed); `cq-map`'s replay uses `World` with no goal (`kind=None` → no fix, no maybes).
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `refactor(ai): World - the run's pure state, handed out as a SkillInput (LLD §19.5)`.

### Task 6: Perception — one loop for every tool

**Files:**
- Create: `ai-engine/src/conqueror_ai/app/perception.py`
- Modify: `app/map.py` (`run_live` on `Perception`), `app/objects.py` (`run_live` on `Perception`), `app/drive.py` (`drive_session` on `Perception`)
- Test: `ai-engine/tests/test_perception.py`; existing `tests/test_map_app.py` live tests, `tests/test_objects_app.py`, `tests/test_drive_app.py` keep passing unchanged

**Interfaces:**
- Produces: `class Perception: __init__(link: BridgeLink, cam: MjpegSource, world: World, det: Detector | None, depth: DepthFn | None, rec: SessionRecorder | None)`; `async run(stop: asyncio.Event) -> None` (the links' tasks and the telemetry task); `async next_picture(timeout_s: float) -> CameraModel | str | None` (one picture through decode, detect + depth off the loop, `world.on_picture`; None on timeout); `state(now: float) -> str | None` (`map_state`); `failure() -> str | None` (`task_failure`); `last_picture_s`, `last_telem_s`; `async close()`.

- [ ] **Step 1: Write the failing tests** (fake bridge, as `test_drive_app.py` does): `test_telemetry_flows_while_the_detector_runs` (a slow fake detector: telemetry samples keep arriving — off-loop); `test_a_crashed_link_is_reported` (`failure()` names it); `test_one_picture_goes_through_once` (two `next_picture` calls with one frame: the second times out → None).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement from `drive_session`'s loop (the depth gather, `see_off_loop`); move `cq-map --live` and `cq-objects --live` onto it; delete their own loops.
- [ ] **Step 4:** `uv run pytest -q` all pass (the live app tests unchanged); ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `refactor(ai): Perception - one camera and telemetry loop for cq-map, cq-objects and cq-drive (LLD §19.5)`.

### Task 7: Driver port and SkillRunner

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/ports.py` (`Driver`), `ai-engine/src/conqueror_ai/app/runner.py`
- Modify: `adapters/drive_link.py` (implements `Driver`), `app/drive.py` (`drive_session` = wiring: `World`, `Perception`, `SkillRunner`, `DriveLink`; CLI prints the status callback)
- Test: `ai-engine/tests/test_runner.py`; `tests/test_drive_app.py` unchanged

**Interfaces:**
- Produces: `class Driver(Protocol)`: `connected: bool`, `refused: str | None`, `async run(stop)`, `async drive(cmd: Command)`, `async cam(pan: float, tilt: float)`, `async finish()`. `class SkillRunner: __init__(world: World, perception: Perception, driver: Driver, log: DecisionLog | None, on_status: Callable[[str, Command, float], None] | None, tick_s: float = TICK_S)`; `async run(skill: Skill) -> tuple[Result, str]` (detail) — ticks on each picture or `tick_s`, skill off the loop, sends cmd and cam, refusal → `REFUSED`, `perception.failure()` → `FAILED`, always `driver.finish()`.
- `DecisionLog` is defined in Task 8; here the runner takes `log=None` and calls `log.tick(...)` only when given (the type is a `Protocol` `TickLog` in `runner.py`: `tick(now_s, inp, out, state, debug) -> None`).

- [ ] **Step 1: Write the failing tests** (Review Focus 5) with a fake driver and a scripted perception/world: `test_a_refusal_ends_refused_and_finishes`; `test_a_failed_link_ends_failed_and_finishes`; `test_the_skills_result_is_passed_through`; `test_a_status_and_a_log_line_each_tick`.
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; `drive_session` becomes wiring; the CLI's printed line and `result` strings are unchanged (`f"{result}"`, `"refused by the tank (...)"`, `"failed - ..."`, `"stale - ..."` built from `Result` + `detail`).
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `refactor(ai): a Driver port and SkillRunner under cq-drive (LLD §19.5)`.

### Task 8: The decisions log and replay

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/decisions.py`
- Modify: `app/drive.py` (`--record DIR` also writes `decisions.jsonl`; `--replay DIR`)
- Test: `ai-engine/tests/test_decisions.py`

**Interfaces:**
- Produces: `class DecisionLog: __init__(path: Path, mission: dict[str, object], seed: int)`; `tick(now_s, inp: SkillInput, out: SkillOut, state: str, debug: dict[str, object]) -> None` (one JSON line; the snapshot's `blocked`, `high`, `seen` as `np.packbits` + base64 with shape, its pose, `t_us`, flags; `costs` not stored — rebuilt from the snapshot, `area` and the cost settings); `close()`. `read_decisions(path) -> tuple[dict, list[LoggedTick]]`; `replay(path, make: Callable[[dict, int], Skill]) -> ReplayReport` with `first_diff: int | None`, `states: list[tuple[float, str]]`. The skill's rng seed is fixed per run and logged.

- [ ] **Step 1: Write the failing tests:** `test_a_logged_sim_run_replays_identically` (a short sim find run logged → `replay(...).first_diff is None`); `test_a_changed_skill_shows_the_first_differing_tick` (Review Focus 4: replay with a skill whose `tick` differs from tick 5 → `first_diff == 5`); `test_a_snapshot_round_trips` (pack → unpack equal arrays).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; `cq-drive --replay DIR` prints the state timeline and "identical" or the first differing tick with both outputs.
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `feat(ai): every drive run's decisions logged; cq-drive --replay (LLD §19.6)`.

### Task 9: Floor check (owner present, STOP in hand)

Before each run: say what moves (the tank drives itself ≤ 25 cm/s, turns on the spot, the camera pans, it may back up ≤ 20 cm over floor it drove on), confirm floor, numbered steps, the camera check first, `--record`.

- [ ] **Step 1:** `cq-drive wander 60 --area 2 --record recordings/z7-w` — no touches; then `cq-drive --replay recordings/z7-w` → identical.
- [ ] **Step 2:** `cq-drive find toy --area 2 --record recordings/z7-f` with the teal car on open floor ~1.5 m off — `done`; replay → identical.
- [ ] **Step 3:** `cq-map --live` and `cq-objects --live` for ~20 s each — the map fills / the car is listed as before.
- [ ] **Step 4:** design-notes §192 (results, how each number was derived); delete the recordings; commit `docs: Plan Z7 floor checks (design-notes §192)`.

### Task 10: As built

- [ ] **Step 1:** AI engine LLD §19.11 "as built": files, every deviation from §19.1-§19.10 with its reason, the class diagram as built.
- [ ] **Step 2:** CLAUDE.md: Plan Z7 in the plans list and the roadmap line.
- [ ] **Step 3:** Commit `docs: Plan Z7 as built (AI engine LLD §19.11, CLAUDE.md)`; push; ask the owner to merge `plan-z7`; then give the branch removal commands.
