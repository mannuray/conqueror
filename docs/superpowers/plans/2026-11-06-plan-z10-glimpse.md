# Plan Z10 — FIND uses glimpses Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** FIND acts on a single flash: sightings in turning pictures count, a sighting while the camera moves gets a second look, the nearest maybe is checked first, and a lost chase checks its last spot from near.

**Architecture:** the object pipeline gives turning pictures' sightings as maybes (not to the tracker or the map) and camera-moving pictures' sightings as `Glimpse`s with a bearing range; `World` passes both in `SkillInput` (`fresh`, `glimpses`), the decisions log records them; `RoamSkill` gains a second-look state and a nearest-first switch; `ChaseSkill(search_when_lost=False)` inside FIND; `LeaseDriver` logs send gaps.

**Tech Stack:** Python 3.12 (uv), pytest, ruff, pyright strict.

**Spec:** AI engine LLD §25 (`docs/lld/ai-engine-lld.md`), design-notes §217. Branch `plan-z10-glimpse` (worktree `scratchpad/plan-z10-glimpse`, on top of `plan-l1-lessons`).

## Global Constraints
- Values: `GATE_DPS` 45, `CAM_RATE_DPS` 20 unchanged; `MAYBE_SCORE` 0.15; `SECOND_LOOK_S` 1.5; `ONE_LOOK_DEG` 50; second-look pan limit 90° (beyond: turn on the spot first); `GLIMPSE_SAME_DEG` 20; `GLIMPSE_FORGET_S` 15; `SECOND_LOOK_GAP_S` 3; `SAME_SPOT_M` 0.5; nearer = < 0.6 × current **and** ≥ 0.5 m less; send-gap log over 0.25 s.
- The room map and the tracker never use turning or camera-moving pictures (their rules stay). A person is never a maybe or a glimpse (`clear(dets)` first).
- `used` keeps its meaning (search "looked at" marks only from used pictures).
- Old recordings replay: new `SkillInput` fields default when absent (`fresh` = `used`, `glimpses` = ()).
- The bridge's 300 ms deadman and every stop rule unchanged.
- `cd ai-engine && uv run pytest -q`, `uv run ruff check .`, `uv run ruff format --check .`, `uv run pyright` clean after every task.
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A turning picture with a box must never add a tracker object or a map mark (only a maybe).
2. Second looks must not loop: the same glimpse direction within 15 s ignored; at most one per 3 s; never during the close look, the chase or a way out.
3. A second look beyond ±90° turns the tank only by the planner's spin check (`can_spin`), like every turn; refused → the look at the nearest allowed pan.
4. Nearest-first must not flip-flop between two candidates (the 0.6× and 0.5 m margins; the old one stays pending, no reopen counted).
5. Replaying an old `decisions.jsonl` (no `fresh` / `glimpses`) gives the same decisions as before.

---

### Task 1: turning pictures' sightings are maybes (A1)

**Files:** Modify `ai-engine/src/conqueror_ai/domain/objects.py` (`FrameGate.camera_turning`, `ObjectPipeline.on_frame`, `fresh()`), `app/world.py` (`_fresh`, `input`), `domain/drive/skill.py` (`SkillInput.fresh`, `make_input`), `adapters/decisions.py`, `domain/drive/roam.py` (sightings from `fresh`). Tests: `tests/test_objects.py`, `tests/test_world.py`, `tests/test_decisions.py`, `tests/test_drive_roam.py`.

**Interfaces:**
- Produces: `FrameGate.camera_turning(cap_us: int) -> CameraModel | None` (the model as `camera_for` without the `GATE_DPS` check; None when `camera_for` gives any reason other than "turning"); `ObjectPipeline.fresh() -> bool` (the newest picture gave maybes: used or turning); `SkillInput.fresh: bool = False` (after `maybes`); `make_input(..., fresh=...)`.

- [ ] **Step 1: Failing tests:** `test_turning_picture_gives_maybes_not_objects` (a pose turning 60°/s, a box detection → `on_frame` returns "turning", `maybes()` has the box at the spot a still picture gives (same pose/angles, rate 0) within 1 cm, `objects()` empty, `fresh()` True); `test_camera_moving_picture_gives_no_maybes` (`maybes()` empty, `fresh()` False); `test_world_input_fresh_after_turning_picture`; `test_decisions_round_trip_fresh` and `test_decisions_old_line_fresh_defaults_to_used`; `test_roam_takes_fresh_maybes_as_candidates` (fresh True, used False → pending candidate).
- [ ] **Step 2:** `uv run pytest -q tests/test_objects.py tests/test_world.py tests/test_decisions.py tests/test_drive_roam.py` → the new ones FAIL (missing attributes).
- [ ] **Step 3:** Implement. In `on_frame`, for cam == "turning": `rough = self.gate.camera_turning(f.cap_us)`; if not None, `self._maybes` = located sure + unsure with `rough` (tracker not updated, `_near` unchanged), fresh True; still return "turning". Roam: `for spot in maybes if inp.fresh else ()`.
- [ ] **Step 4:** the same pytest command → PASS; then the full suite, ruff, pyright.
- [ ] **Step 5: Commit** `feat(ai): FIND takes sightings from turning pictures (AI engine LLD §25 A1)`.

### Task 2: glimpses from camera-moving pictures (A2, the data)

**Files:** `domain/objects.py` (`Glimpse`, `CameraAngles.still_pan_before`, `ObjectPipeline.glimpses()`), `app/world.py`, `domain/drive/skill.py` (`SkillInput.glimpses`), `adapters/decisions.py`. Tests: `tests/test_objects.py`, `tests/test_world.py`, `tests/test_decisions.py`.

**Interfaces:**
- Produces: `@dataclass(frozen=True) class Glimpse: cls: str; at: tuple[float, float]; lo_rad: float; hi_rad: float; t_us: int` (map bearings, lo ≤ hi, unwrapped so hi − lo ≤ π); `CameraAngles.still_pan_before(t_us: int) -> float | None` (pan of the newest sample before t whose rate was ≤ `CAM_RATE_DPS` and without CAM_MOVING); `ObjectPipeline.glimpses() -> list[Glimpse]` (the newest picture's); `SkillInput.glimpses: tuple[Glimpse, ...] = ()`; `World` collects the goal kind's glimpses since the last `input()`.
- Bearing for pan p (degrees from centre, + right, after `from_servo`): `th(t) - radians(p) - atan((u - cx) / fx)`, u = box middle; lo / hi over p ∈ [A, B], A = `still_pan_before`, B = the pan at t. `real_pan_deg` / servo gain as `CameraModel` uses them.

- [ ] **Step 1: Failing tests:** `test_glimpse_range_from_move_start_to_reported_pan` (heading 90°, still pan 0 then reported +30 right, box at image centre → lo 60°, hi 90° within 0.5°); `test_glimpse_at_right_edge_shifts_both_ends` (by atan((w-1-cx)/fx)); `test_no_glimpse_without_pose`; `test_no_glimpse_for_a_person`; `test_world_collects_glimpses_of_the_goal_kind_since_last_input` (two pictures, one input → both; next input → none); `test_decisions_round_trip_glimpses`, old line → ().
- [ ] **Step 2:** run them → FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** PASS; full suite, ruff, pyright.
- [ ] **Step 5: Commit** `feat(ai): glimpses with a bearing range from pictures taken while the camera moves (LLD §25 A2)`.

### Task 3: the second look (A2, roam)

**Files:** `domain/drive/roam.py`. Test: `tests/test_drive_roam.py`.

**Interfaces:**
- Consumes: `SkillInput.glimpses` (Task 2).
- Produces: roam state `"second"`; `debug()["second"]` = the bearings left to look at (degrees) or None; constants `SECOND_LOOK_S = 1.5`, `ONE_LOOK_DEG = 50.0`, `SECOND_LOOK_PAN_DEG = 90.0`, `GLIMPSE_SAME_DEG = 20.0`, `GLIMPSE_FORGET_S = 15.0`, `SECOND_LOOK_GAP_S = 3.0`.

- [ ] **Step 1: Failing tests** (driving find roam, helpers like the existing ones):
  - `test_glimpse_left_stops_and_looks_there` - range 30°-60° left of the heading → STOP, cam pan −45; 1.5 s of still used pictures; then back to "drive".
  - `test_a_maybe_in_the_second_look_is_a_candidate` - during the look a fresh maybe → pending candidate.
  - `test_wide_glimpse_gets_two_looks` - range 80° wide → looks at both ends in turn.
  - `test_glimpse_behind_turns_first` - range middle 150° left → a spin command first, then the look within 90°.
  - `test_same_glimpse_again_is_ignored` (within 15 s, from within 0.5 m) and `test_glimpse_30_degrees_away_is_looked_at` (after 3 s).
  - `test_no_second_look_during_close_look_chase_or_way_out`.
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** Implement: in `_tick`, after the chase hand-over and before candidates/check/drive, when `self.find` and state in ("drive", "check" with phase "closer") and not recovering: take the newest glimpse not ruled by the memory rules → state "second" (remember the state to go back to and keep `_check`). Each look: aim (relative pan = −degrees(wrap(bearing − th))); |pan| > 90 → spin toward it if `planner.can_spin` else clamp to ±90; still pictures `SECOND_LOOK_S` (as `_checking`'s `still_t0`); `_SecondLook(bearings, i, t0)` dataclass.
- [ ] **Step 4:** PASS; full suite, ruff, pyright.
- [ ] **Step 5: Commit** `feat(ai): FIND takes a second look where a glimpse may be (LLD §25 A2)`.

### Task 4: the nearest maybe first (B)

**Files:** `domain/drive/roam.py`. Test: `tests/test_drive_roam.py`.

- [ ] **Step 1: Failing tests:** `test_a_much_nearer_candidate_takes_over_the_check` (checking one 3 m away, phase closer; a new pending one 0.6 m away → `debug()["check"]` is the near one, the far one still pending, its reopens unchanged); `test_a_slightly_nearer_one_does_not` (2.5 m vs 3 m → no switch); `test_no_switch_during_the_look`.
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** Implement `_nearer(at, current) -> Candidate | None` (straight-line distance; < 0.6 × and ≥ 0.5 m less); on a switch the current goes back to pending without counting a try.
- [ ] **Step 4:** PASS; full suite, ruff, pyright.
- [ ] **Step 5: Commit** `feat(ai): FIND checks a much nearer maybe first (LLD §25 B)`.

### Task 5: a lost chase checks from near (C)

**Files:** `domain/drive/chase.py` (`search_when_lost: bool = True`), `domain/drive/roam.py` (`ChaseSkill(..., search_when_lost=False)` both places). Tests: `tests/test_drive_chase.py`, `tests/test_drive_roam.py`, `tests/test_drive_roam_sim.py` if a sim test expects the in-place sweep after a loss.

- [ ] **Step 1: Failing tests:** `test_chase_without_search_ends_lost_at_once` (fix gone → result `lost` that tick, no camera sweep); `test_plain_chase_still_sweeps` (default True unchanged); `test_find_loses_the_chase_and_checks_its_last_spot_next` (roam: chase loses → state "drive"/"check" with the last seen spot as the check within 2 ticks, no 20 s sweep).
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** Implement: in `_approach`, `if fix is None or self._lost(fix)`: `if not self.search_when_lost: return self._end(Result.LOST)` (keep `_target` for `last_seen`). A chase that starts with no fix inside FIND (state "start") still looks round as today.
- [ ] **Step 4:** PASS; full suite, ruff, pyright. A sim test that relied on the sweep after a loss: re-check what the owner asked (§217) and ledger a ruling.
- [ ] **Step 5: Commit** `feat(ai): a lost chase inside FIND checks its last spot from near (LLD §25 C)`.

### Task 6: the send-gap log (§25.6)

**Files:** `adapters/lease_driver.py`. Test: `tests/test_lease_driver.py`.

- [ ] **Step 1: Failing test** `test_a_send_gap_over_a_quarter_second_is_logged` (a fake link whose `drive` awaits 0.3 s once → one log line naming the gap in ms; gaps under 0.25 s → none). Use the module's logger (`logging.getLogger(__name__)`, caplog) or the print style the serve log uses - follow what `skill_link.py` does.
- [ ] **Step 2:** FAIL. **Step 3:** in `_send`, before stamping: `gap = now - self._sent_s` when a frame was sent before; over `SEND_GAP_LOG_S = 0.25` → one line `skill link: {gap*1000:.0f} ms between drive frames`. **Step 4:** PASS; suite, ruff, pyright.
- [ ] **Step 5: Commit** `feat(ai): log drive-frame gaps over 250 ms (LLD §25.6)`.

### Task 7: sim, docs as built, floor
- [ ] **Step 1:** in `tests/test_drive_roam_sim.py`, if the sim world can mark pictures as turning: `test_a_box_seen_only_while_turning_is_found`; else a ledger ruling (covered by Task 1's roam test).
- [ ] **Step 2:** LLD §25.7 as built, design-notes §217 "Built", CLAUDE.md plans row; commit `docs: Plan Z10 as built (AI engine LLD §25.7, design-notes §217)`, push.
- [ ] **Step 3 (owner present, tank on the floor, STOP in hand, camera check):** restart the engine on `room4`; `AREA 0  FIND "box` from where the box flashes while it turns; a box near while a remembered one is far; a lost chase. Results into §217.
