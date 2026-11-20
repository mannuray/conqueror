# Plan G4 - ESCAPE runs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (owner's standing rule: native) to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** ESCAPE backs off at once, turns and runs, then searches back - quick (design-notes §254).

**Architecture:** the engine's `EscapeSkill` is rewritten as back → run → search (AI engine LLD §28.24); `PersonWatch`
keeps an out-of-view person's place from the pose now; the bridge lets a run drive forward out of view for 3 s when
the person is behind (bridge LLD R34.15). Branch `plan-g4-escape-runs`, on top of `plan-g3-game-words`.

**Tech Stack:** Python 3.12 (uv, pytest, ruff, pyright strict); C++ ESP32 bridge (PlatformIO native tests); Tank Logo
word spec (`logogen`).

**Spec:** Spec 10 (`docs/superpowers/specs/2026-11-08-10-people-games-design.md`, the 2026-11-20 correction), AI
engine LLD §28.24, bridge LLD R34.15, design-notes §254.

## Global Constraints

- Game cap 25 cm/s (owner 2026-11-08) - unchanged.
- S7 / S11 relaxed for ESCAPE only; Logo `BACK` in a game still asks `back_ok`.
- Out of view forward: `|bearing|` >= 90, for 3 s from the first such forward move.
- Backing stays straight at `v_min` (18 cm/s) - the ramp's rule.
- Numbers: `BACK_OFF_M` 0.30, `WALL_S` 0.5, `WALL_M` 0.02, `WALL_DEG` 5, `RUN_MAX_S` 2.8 (of forward driving), run turns (0, 30, -30, 60, -60, 90, -90).
- Generated files are never edited by hand (`logogen write`).

## Review Focus

1. A wall behind: the back-off must end on no progress, never keep pushing until the Uno's stall (grabbed).
2. The run with the person still in view ahead (a narrow room): the keep-out must still end a forward move towards
   them (`|bearing|` < 90 is held out of view, and in view `KeepOut::check` applies).
3. Out-of-view bearing with no pose (the pose filter not ready): keep the sighting's, never 0.
4. A catch during the back-off or run: the skill ends `caught` through the bridge as before.
5. The search finding nobody, or its turn held by the bridge (a wall ahead): `lost`, never waiting for ever.

---

### Task 1: The person out of view keeps its place (engine)

**Files:** Modify `ai-engine/src/conqueror_ai/domain/person.py`, `ai-engine/src/conqueror_ai/app/world.py`,
`ai-engine/src/conqueror_ai/app/game_serve.py`; Test `ai-engine/tests/test_person.py`.

**Produces:** `PersonWatch.update(boxes, cam, sonar_m, t_us, at: XYTh | None = None)`, `PersonWatch.latest(now_us,
at: XYTh | None = None) -> PersonEst | None`; `person_message(watch, now_us, at=None)`.

- [ ] Test `test_out_of_view_bearing_follows_the_turn`: seen 1.0 m ahead at pose (0,0,0); latest at pose (0,0,π)
      → seen False, `abs(bearing_deg)` > 170, `d_m` ≈ the front edge's distance to the stored point.
- [ ] Test `test_out_of_view_without_a_pose_keeps_the_sighting`.
- [ ] Run: FAIL. Implement (store the floor point from the pessimistic distance and the front edge). World and the
      person stream pass the pose. Run: PASS. Commit `feat(ai): an out-of-view person keeps its place`.

### Task 2: ESCAPE back → run → search (engine)

**Files:** Modify `ai-engine/src/conqueror_ai/domain/drive/game_skills.py`, `ai-engine/src/conqueror_ai/app/game_serve.py`;
Test `ai-engine/tests/test_game_skills.py`.

**Produces:** `EscapeSkill(lim, run_m)`; `Progress` (the wall rule).

- [ ] Tests (LLD §28.24's list): `test_escape_backs_off_at_once_over_unknown_floor`, `test_escape_wall_ends_the_back_off`,
      `test_escape_runs_straight_away_when_open`, `test_escape_run_takes_the_next_open_direction`,
      `test_escape_no_stand_between_back_and_run`, `test_escape_spin_is_not_a_wall`,
      `test_escape_camera_looks_ahead_on_the_run`, `test_escape_run_max_counts_forward_only`,
      `test_escape_held_search_turn_ends_lost`, `test_escape_no_open_way_is_no_way`,
      `test_escape_search_done_when_seen`, `test_escape_search_lost`, `test_escape_near_still_backs_off`.
      Old ESCAPE tests that pin the steps / trusted floor / near no_way are replaced (the owner's decision changes
      what they pin), each removal named in the commit.
- [ ] Run: FAIL. Implement. Run the whole engine suite, ruff, pyright. Commit `feat(ai): ESCAPE runs`.

### Task 3: The bridge lets the run go on out of view

**Files:** Modify `firmware/esp32/src/core/game_mode.h/.cpp`; Test `firmware/esp32/test/test_game_mode/test_main.cpp`.

- [ ] Tests (R34.15's list). Run `pio test -e native -f test_game_mode`: FAIL. Implement `runAway`. PASS, the whole
      native suite. Commit `feat(bridge): a run away out of view`.

### Task 4: The word and the docs

**Files:** `logo/spec/words.yaml` (ESCAPE `does`, example `ESCAPE "baby 80`), then `cd logo && uv run logogen write`;
`docs` as-built notes (LLD §28.24, R34.15).

- [ ] `logogen check` clean; web tests 153 pass; build `pio run -e bridge` (the size check). Commit `docs/feat(logo)`.

### Task 5: Floor check (owner's "floor go" first)

- [ ] Flash (OTA app + source), the helper on the branch, `escape_game.logo` with `ESCAPE "person 80`: back-off at once,
      one smooth turn, run, search back; a wall behind ends the back-off without a catch. Results in design-notes.
