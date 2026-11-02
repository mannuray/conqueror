# Plan Z8 — the home spot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A run started with `--home` from a tape mark begins with what earlier runs learned (obstacles weakened and fading, remembered things pruned) and saves at the end; and the viewer shows what the engine thinks (route, destination, candidates, looked-at floor), live and from a replay.

**Architecture:** A pure `Home` (domain) turns a run's room map and memory into what is kept and back into a run's starting map and memory; `HomeStore` (adapter) writes and reads `ai-engine/homes/<name>/` atomically; `cq-drive --home` loads into Plan Z7's `World(room=, memory=)` and saves in its `finally`. A pure `skill_overlay(snap, debug)` paints a skill's notes onto the map picture; `RerunView.skill` logs it; `cq-drive --view` and `--replay DIR --view` use it.

**Tech Stack:** Python 3.12, uv, numpy (`np.savez_compressed`), rerun-sdk 0.39, pytest, ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §20 (approved with fade A), design-notes §189 step 2, §191 decision 1, §193, §194. Builds on Plan Z7 (LLD §19; branch `plan-z8` from `plan-z7`).

## Global Constraints

- **Safety (§191 decision 1):** free-floor evidence is never loaded; `seen` comes back only as `RoomMap.seen_before`, which no `CostMap`, goal or `Recovery` reads (backing goes only over floor known free this run); a loaded obstacle is at most `blocked + |miss|` = 1.1 (then the fade); the Uno's and the engine's stop rules unchanged.
- **Fade A (owner):** each load lowers a loaded obstacle by `FADE_PER_LOAD` 0.2 (1.1 → 0.9 → 0.7 = not blocked).
- **Memory pruning:** at save, gone things dropped, things not seen in the last `KEEP_RUNS` 5 runs dropped, at most `KEEP_PER_WORD` 5 per word (the most recent by `seen_wall_s`).
- **The file:** `homes/<name>/map.npz` (`low`, `high`, `seen`) + `homes/<name>/home.json` `{"version": 1, "frame": {"kind": "home", "pose_offset": [0, 0, 0], "cell_m", "size_m"}, "runs": [...], "things": [...]}`; another version, `cell_m` or `size_m` is refused with a plain message; written atomically (temp file, then rename).
- `ai-engine/homes/` git-ignored; homes, recordings and decision logs stay local (they show the home) and recordings are deleted after use.
- Python 3.12 via uv; type hints; `uv run ruff check .`, `uv run ruff format --check .`, `uv run pyright` clean — **run unpiped**, each exit 0 before a commit. `domain/` never imports `app/` / `adapters/`, reads no clock, prints nothing.
- Commits: Conventional Commits, ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Branch `plan-z8`.

## Review Focus

1. A loaded home must never make floor known-free (the recovery backs only over this run's floor) — test in Task 1.
2. A half-written or corrupt home (power off during the save) is refused plainly, never a crash, and never half-loaded — test in Task 2.
3. Many runs must not grow the home without bound (memory pruned, obstacles fade) — test in Task 1.
4. Ctrl-C or a crash mid-run still saves the home, and a save that fails says so without hiding the run's result — test in Task 3.
5. `--view` must never slow the drive's ticks past the drive link's freshness (drawing off the decision path) — test in Task 4.

---

### Task 1: `Home` — what is kept, and how it comes back

**Files:**
- Create: `ai-engine/src/conqueror_ai/domain/home.py`
- Modify: `ai-engine/src/conqueror_ai/domain/room_map.py` (`seen_before` layer)
- Test: `ai-engine/tests/test_home.py`

**Interfaces:**
- Produces: `RoomMap.seen_before: NDArray[np.bool_]` (zeros at birth; nothing that drives reads it). `FADE_PER_LOAD = 0.2`, `KEEP_RUNS = 5`, `KEEP_PER_WORD = 5`, `HOME_VERSION = 1`. `@dataclass(frozen=True) class Home`: `low`, `high`, `seen` (arrays), `cell_m`, `size_m`, `runs: tuple[str, ...]`, `things: tuple[Remembered, ...]`; `Home.from_run(room: RoomMap, memory: ThingMemory, earlier: Home | None, run_id: str) -> Home` (keeps `low` / `high` values above `settings.blocked`, else 0; `seen` = this run's `seen` or the earlier `seen`; `runs` = earlier runs + `run_id`, last `KEEP_RUNS`; things pruned); `Home.room_for_run(settings: MapSettings) -> RoomMap` (each kept value → `min(v, blocked + |miss|) - FADE_PER_LOAD`, values ≤ `blocked` dropped to 0; `seen` empty; `seen_before` = kept `seen`); `Home.memory() -> ThingMemory`.

- [ ] **Step 1: Write the failing tests:**
  - `test_an_obstacle_comes_back_weakened_and_fades` — a run's `low` 4.0 (clamped high) at a cell: `room_for_run` gives 0.9 (1.1 - 0.2); saved and loaded again without being seen: 0.7 → no longer blocked (`> blocked` false); two misses clear a loaded 0.9 below blocked.
  - `test_free_floor_and_weak_evidence_never_come_back` — a `low` of -2.0 (floor seen) and 0.5 (below blocked) → 0 after a load.
  - `test_a_loaded_home_never_makes_floor_known_free` (Review Focus 1) — load a home whose `seen` is all true: the run's `room.seen` is all false, `seen_before` all true, `CostMap(snapshot).known_free` all false, and `Recovery`'s back check (`_back_ok`) refuses backing.
  - `test_explore_and_search_see_a_fresh_map` — `ExploreGoal` / `SearchGoal` destinations on a loaded room equal those on a fresh room with the same marks.
  - `test_memory_is_pruned` (Review Focus 3) — gone dropped; a thing whose `run_id` is not in the last 5 runs dropped; 7 balls → the 5 most recent kept; 20 saves in a row keep `runs` at 5.
- [ ] **Step 2:** `uv run pytest tests/test_home.py -q` — Expected: FAIL (no module `home`).
- [ ] **Step 3:** Implement `domain/home.py` and the `seen_before` layer (`RoomMap.__init__`; `snapshot` does not copy it).
- [ ] **Step 4:** `uv run pytest -q` — all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `feat(ai): Home - obstacles kept weakened and fading, memory pruned, seen only as seen_before (LLD §20.2-§20.4)`.

### Task 2: `HomeStore` — the files

**Files:**
- Create: `ai-engine/src/conqueror_ai/adapters/home_store.py`
- Modify: `ai-engine/.gitignore` (`homes/`)
- Test: `ai-engine/tests/test_home_store.py`

**Interfaces:**
- Consumes: `Home` (Task 1).
- Produces: `class HomeStore: __init__(root: Path)`; `load(name: str) -> Home | None` (None: no such home); `save(name: str, home: Home) -> None` (both files written to `*.tmp` then `os.replace`, the JSON last); `class HomeError(ValueError)` with a plain message (`"home 'kitchen': another map size (6.0 m) - start fresh or delete ai-engine/homes/kitchen"`). `HOMES = Path(...)/"homes"` in `app/drive.py`.

- [ ] **Step 1: Write the failing tests:** `test_a_home_round_trips` (save → load: equal arrays, runs, things); `test_no_home_is_none`; `test_another_version_or_size_is_refused` (`HomeError`, message names the home); `test_a_half_written_home_is_refused_plainly` (Review Focus 2: a truncated `map.npz`, a truncated `home.json`, a `.tmp` left over → `HomeError`, never another exception; the earlier good home stays readable when only a `.tmp` is broken); `test_homes_are_git_ignored` (`git check-ignore ai-engine/homes/x`).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `feat(ai): HomeStore - homes/<name>/ written atomically, refused plainly when wrong (LLD §20.5)`.

### Task 3: `--home` on cq-drive and cq-map

**Files:**
- Modify: `ai-engine/src/conqueror_ai/app/drive.py` (`--home [NAME]`; `Kit.home: tuple[HomeStore, str] | None`), `ai-engine/src/conqueror_ai/app/map.py` (`--home NAME` with `--live`: load only)
- Test: `ai-engine/tests/test_drive_app.py`, `ai-engine/tests/test_map_app.py`

**Interfaces:**
- Consumes: `HomeStore`, `Home` (Tasks 1-2); Z7's `World(..., room=, memory=)`.
- Produces: `start_home(store: HomeStore, name: str, settings: MapSettings) -> tuple[RoomMap | None, ThingMemory | None, Home | None, str]` (the message: `"home 'home': 3 runs, 41 obstacle cells, 2 things - put the tank on the home mark, facing the way it faced last time"` or `"home 'home': new"`); `end_home(store, name, earlier: Home | None, room: RoomMap, memory: ThingMemory, run_id: str) -> str` (`"home 'home' saved"` or the failure). `drive_session` calls `end_home` in its `finally` (after the stop); its message is printed after the result.

- [ ] **Step 1: Write the failing tests:** `test_a_home_run_starts_from_the_home_and_saves_it` (fake bridge as `test_drive_run_never_moves_without_a_fresh_map`, a stored home with one obstacle and one remembered ball: the run's `World` room holds the weakened obstacle and the memory the ball; after the run the home has 2 runs); `test_a_cancelled_run_still_saves` (Review Focus 4: the `drive_session` task cancelled mid-run → the home saved); `test_a_failed_save_is_said_and_the_result_kept` (a store whose `save` raises → the result string unchanged, the message says the save failed); `test_cq_map_live_shows_a_home` (`run_live` with a loaded room: its first snapshot holds the obstacle); `test_a_home_run_replays_identically` (a recorded `--home` run with a remembered spot: `replay_text` says identical).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; `main` prints the start message before connecting and the save message after the result.
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `feat(ai): cq-drive --home loads and saves the home; cq-map --home shows it (LLD §20.1)`.

### Task 4: Seeing what the engine thinks

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/roam.py` (`debug()`), `domain/drive/chase.py` (`debug()`), `domain/drive/goals.py` (`SearchGoal.looked_cells()`), `adapters/rerun_view.py` (`skill_overlay`, `RerunView.skill`), `app/drive.py` (`--view`; `--replay DIR --view`), `app/runner.py` (a `TickView` hook called after the drive command is sent)
- Test: `ai-engine/tests/test_rerun_view.py` (or the existing view test file), `tests/test_drive_roam.py`, `tests/test_runner.py`, `tests/test_decisions.py`

**Interfaces:**
- Produces: `RoamSkill.debug()` adds `"route": list[XY] | None`, `"ruled_out": list[XY]`, `"check": XY | None`, `"looked": list[XY]` (find only; cell centres); `ChaseSkill.debug()` adds `"target": XY | None`. `SearchGoal.looked_cells() -> list[XY]`. `skill_overlay(snap: MapSnapshot, debug: dict[str, object]) -> NDArray[np.uint8]` (the `map_image` picture with: looked-at floor faint green, the route a blue line, the destination blue, pending candidates yellow, ruled-out grey, the check spot magenta; points off the map ignored). `RerunView.skill(t_us, snap, debug)` logs it as `map/skill` and the state as text. `SkillRunner(..., view: TickView | None = None)`: `view.tick(now_s, inp, out, state, debug)` called after `driver.drive` (never before it).

- [ ] **Step 1: Write the failing tests:** `test_debug_carries_the_route_the_candidates_and_the_looked_floor`; `test_the_overlay_paints_each_note` (a snapshot and a debug dict → the expected colours at the expected cells; a point off the map is ignored); `test_the_view_is_called_after_the_command_is_sent` (Review Focus 5: a fake driver and a slow fake view - `drive` is called before `view.tick` each tick, and a view that raises doesn't end the run); `test_replay_view_draws_every_logged_tick` (a logged sim run, a fake view: one call per tick).
- [ ] **Step 2:** run — Expected: FAIL.
- [ ] **Step 3:** Implement; a view exception is caught and counted (printed once), never ends a run.
- [ ] **Step 4:** `uv run pytest -q` all pass; ruff, format, pyright exit 0.
- [ ] **Step 5: Commit** `feat(ai): the viewer shows what the engine thinks - route, candidates, looked-at floor; cq-drive --view (LLD §20.7)`.

### Task 5: Floor checks (owner present, STOP in hand)

Before each run: say what moves (the tank drives itself ≤ 25 cm/s, turns on the spot, the camera pans, it may back up ≤ 20 cm over floor it drove on), confirm floor, numbered steps, the camera check first, `--record`, `--view`. Tape the home mark first; the tank on it, facing the same way, for every `--home` run.

- [ ] **Step 1:** `cq-drive explore --home --area 3 --record recordings/z8-e1 --view` — `done`, ≥ 80 % of the reachable floor seen (spec 9 §6.4; `cq-map` replay + the owner's eye).
- [ ] **Step 2:** `cq-map --live --home home` from the mark — the furniture from run 1 shown before driving; then `cq-drive wander 60 --home --area 3 --record recordings/z8-w --view` — no touches, the marked furniture driven round.
- [ ] **Step 3:** the teal car placed where run 1's camera saw it (or let a run see it), then `cq-drive find toy --home --area 3 --record recordings/z8-f --view` — the remembered spot checked first; `done`.
- [ ] **Step 4:** spec 9 §6.2: a board across two boxes at ~18 cm — `cq-drive wander 60 --area 2 --record recordings/z8-o --view`: marked high, never driven under.
- [ ] **Step 5:** spec 9 §6.5: a box between the tank and the teal car — `cq-drive find toy --area 2 --record recordings/z8-b --view`: round the box, `done`, no touch.
- [ ] **Step 6:** `cq-drive --replay DIR` on each — identical. Design-notes §195 (results, how each number was derived); delete the recordings and the test home; commit `docs: Plan Z8 floor checks (design-notes §195)`.

### Task 6: As built

- [ ] **Step 1:** AI engine LLD §20.9 "as built": files, every deviation with its reason, the class diagram as built.
- [ ] **Step 2:** CLAUDE.md: Plan Z8 in the plans list and the roadmap line.
- [ ] **Step 3:** Commit `docs: Plan Z8 as built (AI engine LLD §20.9, CLAUDE.md)`; push; ask the owner to merge `plan-z8` (after `plan-z7`); then give the branch removal commands.
