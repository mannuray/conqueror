# Plan Z9 — FIND looks from near and gives a maybe another chance — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** FIND checks every maybe from 50 cm, opens a ruled-out spot again when it is seen again (at most twice), confirms on 2 pictures during a close look, and leaves a candidate where a chase lost the thing.

**Architecture:** Engine only (`ai-engine/`, domain layer). `Candidates` gains reopening (A) and routes to 0.5 m (C); `RoamSkill` checks from 0.5 m (C), counts close sightings during a look and hands the chase a fix made from maybes (B), and turns a lost chase's last seen spot into a candidate (D). No page, bridge, Logo or protocol change.

**Tech Stack:** Python 3.12 via uv, pytest, ruff, pyright strict.

**Spec:** `docs/lld/ai-engine-lld.md` §24 (design-notes §215). Branch `plan-z9-find`, worktree `scratchpad/plan-z9-find`.

## Global Constraints

- `CHECK_CLOSE_M = 0.5` — a check looks from within 0.5 m of the spot, tank centre (`CHECK_NEAR_TOL_M` 0.05 kept); lives in `domain/reach.py` beside `SURE_REACH_M` (both roam and candidates use it).
- `REOPEN_MAX = 2` — a ruled-out candidate is opened again by a sighting within `SAME_SPOT_M` (0.5) at most this many times; then sightings near it are ignored.
- `CHECK_HITS = 2` — pictures (`inp.used`) during a check's look phase with a maybe within `SAME_SPOT_M` of the spot that confirm it.
- The made-up fix is `TargetFix(oid=-1, x, y, seen_age_s)`; the tracker's fix, when present, always wins.
- Unchanged and not to be weakened: `MAYBE_SCORE` 0.15, the tracker's `CONFIRM_HITS` 3 / `CONFIRM_S` 0.5, `SURE_REACH_M` 1.0 for the search's looked-at floor, `CHECK_LOOK_S` 1.5, `CHECK_TRIES` 3, `LOST_AFTER_S` 3.0, the chase's stop distance, a person never a candidate, maybes outside the play area never candidates, stale map / sonar `clear_m` stops.
- Python: type hints everywhere, ruff + pyright strict clean; match the files' comment style (short `#` notes citing §24 / §215).
- Tests are changed only where §24 changes the behaviour they pin (named in each task); no test is loosened otherwise.
- Commits: Conventional Commits ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. A maybe flickering at a spot just ruled out from near: reopened and checked again at most `REOPEN_MAX` times, never an endless check loop (Task 1, Task 4 tests).
2. One picture read on several ticks must count as one close sighting, not two (Task 3 test).
3. A maybe of the thing elsewhere (beyond `SAME_SPOT_M` of the spot) during a look does not count toward the spot's hits (Task 3 test).
4. The tracker's fix arriving while the made-up fix drives the chase replaces it (Task 3 test).
5. A chase lost at a spot outside the play area leaves no candidate (Task 4 test).

---

### Task 1: Candidates reopen and route to 0.5 m (A, C's route)

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/reach.py` (add `CHECK_CLOSE_M`)
- Modify: `ai-engine/src/conqueror_ai/domain/drive/candidates.py` (`Candidate.reopens`, `REOPEN_MAX`, `add`, `route`/`next` `near_m`)
- Test: `ai-engine/tests/test_drive_candidates.py`, `ai-engine/tests/test_drive_roam.py`

**Interfaces:**
- Produces: `reach.CHECK_CLOSE_M: float = 0.5`; `candidates.REOPEN_MAX: int = 2`; `Candidate.reopens: int = 0`; `Candidates.add(spot: XY) -> None` (unchanged signature, new rule); `Candidates.route(...)` routes with `near_m=CHECK_CLOSE_M`.

- [ ] **Step 1: Write the failing tests** (replace `test_a_ruled_out_spot_ignores_later_sightings`, which pinned the old rule §24 changes)

```python
def test_a_ruled_out_spot_seen_again_opens_again_at_most_twice() -> None:
    c = Candidates()
    c.add((1.0, 0.0))
    (k,) = c.pending()
    for n in (1, 2):
        c.rule_out(k)
        c.add((1.2, 0.1))
        assert c.pending() == [k] and k.reopens == n and k.spot == (1.2, 0.1)
    c.rule_out(k)
    c.add((1.1, 0.0))  # a third time: ignored (REOPEN_MAX 2)
    assert c.pending() == [] and k.reopens == REOPEN_MAX


def test_a_route_to_a_candidate_ends_within_half_a_metre() -> None:
    # open room, candidate 2 m ahead: the route's last point within CHECK_CLOSE_M (+ one cell) of the spot, not 1 m
    ...  # build with this file's room() helper and the routes object the other route tests use;
    assert math.dist(route[-1], spot) <= CHECK_CLOSE_M + 0.05
```
In `test_drive_roam.py::test_a_candidate_not_confirmed_is_ruled_out_and_the_search_goes_on`, the last two lines pinned "seen weakly there again: ignored"; change them to: seen again → `s.state == "check"` (A).

- [ ] **Step 2: Run to see them fail**

Run: `cd ai-engine && uv run pytest -q tests/test_drive_candidates.py tests/test_drive_roam.py -k "opens_again or half_a_metre or not_confirmed"`
Expected: FAIL (`REOPEN_MAX` not defined / pending empty / route ends ~1 m / state not check).

- [ ] **Step 3: Implement** — `CHECK_CLOSE_M = 0.5` in `reach.py` with a comment citing §215 (owner: 40-50 cm from the lens); in `Candidates.add`, a nearest candidate that is `ruled_out` with `reopens < REOPEN_MAX` becomes `pending`, `reopens += 1`, spot moved to the sighting; `route` and `next` use `near_m=CHECK_CLOSE_M` and their docstrings say so.

- [ ] **Step 4: Run the tests** — same command. Expected: PASS. Then `uv run pytest -q tests/test_drive_candidates.py tests/test_drive_roam.py` — any other failure is a test pinning 1 m: fix only those named in Task 2.

- [ ] **Step 5: Commit** `feat(ai): a ruled-out FIND spot seen again is checked again, at most twice; candidates are reached to 0.5 m (LLD §24 A, C)`

### Task 2: A check looks from 0.5 m (C)

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/roam.py` (`CHECK_CLOSE_M` from `reach`, no longer `SURE_REACH_M`)
- Test: `ai-engine/tests/test_drive_roam.py`

**Interfaces:**
- Consumes: `reach.CHECK_CLOSE_M` (Task 1).

- [ ] **Step 1: Write the failing test** — in `test_a_far_candidate_is_ruled_out_only_after_a_look_from_near` change the pinned `nearest <= 1.05` to `nearest <= CHECK_CLOSE_M + 0.05` (§24 C); add:

```python
def test_a_check_from_one_metre_drives_to_half_a_metre_before_its_look() -> None:
    s, tank = finder(), SimTank()
    t = until_searching(s, tank)
    spot = (tank.at[0] + 1.0 * math.cos(tank.at[2]), tank.at[1] + 1.0 * math.sin(tank.at[2]))
    ticks(s, tank, t, 1, maybes=[spot])
    # run the check (no sightings): it must reach CHECK_CLOSE_M + 0.05 of the spot before the candidate is ruled out
    ...
    assert nearest <= CHECK_CLOSE_M + 0.05 and s.candidates.ruled_out()
```

- [ ] **Step 2: Run** `uv run pytest -q tests/test_drive_roam.py -k "half_a_metre or far_candidate"` — Expected: FAIL (nearest ~1.0).
- [ ] **Step 3: Implement** — `CHECK_CLOSE_M` imported from `reach`; drop the roam-local alias to `SURE_REACH_M`; the check's time budget keeps `CHECK_S_PER_M` per metre beyond `CHECK_CLOSE_M`.
- [ ] **Step 4: Run** `uv run pytest -q tests/test_drive_roam.py tests/test_drive_roam_sim.py` — Expected: PASS.
- [ ] **Step 5: Commit** `feat(ai): FIND checks a maybe from 50 cm (LLD §24 C, owner 40-50 cm)`

### Task 3: Two close sightings confirm (B)

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/roam.py` (`CHECK_HITS`, `_Check.hits`, `_Check.last_maybe`, `_Check.last_used_s` or equivalent, `RoamSkill._maybe_fix`)
- Test: `ai-engine/tests/test_drive_roam.py`

**Interfaces:**
- Produces: `roam.CHECK_HITS: int = 2`; `RoamSkill._maybe_fix: TargetFix | None`; the chase receives `fix if fix is not None else self._maybe_fix`.

Rules (§24.5): a hit is a **used** picture (`inp.used` true) during the look phase with any maybe within `SAME_SPOT_M` of the check's spot; at `CHECK_HITS` hits the check ends confirmed: `_maybe_fix = TargetFix(-1, *last_maybe, 0.0)` and the state goes to chase (the check's candidate stays as `self._check.cand`, so a later loss is handled by Task 4). While `_maybe_fix` is set and the tracker has no fix: each maybe within `SAME_SPOT_M` of it moves it there with age 0; otherwise its age grows by the tick's `dt`. A tracker fix replaces it (`_maybe_fix = None`). Cleared when the chase ends.

- [ ] **Step 1: Write the failing tests**

```python
def test_two_pictures_with_the_thing_during_a_close_look_hand_over_to_the_chase() -> None:
    # check at a spot already within CHECK_CLOSE_M: two used pictures with a maybe at the spot -> state "chase"
    assert s.state == "chase"

def test_one_picture_during_a_close_look_is_not_enough() -> None:
    # one used picture with the maybe, then none: the look ends and the spot is ruled out
    assert s.state != "chase" and s.candidates.ruled_out()

def test_the_same_picture_on_two_ticks_counts_once() -> None:
    # maybe present on 2 ticks with used=True then used=False: one hit only, no chase
    assert s.state != "chase"

def test_a_sighting_away_from_the_spot_is_no_hit() -> None:
    # two used pictures with a maybe 0.8 m from the check's spot: no chase from them
    assert s.state != "chase"

def test_the_trackers_fix_replaces_the_made_up_one() -> None:
    # after B hands over, a TargetFix(7, ...) arrives: the chase's target is the tracker's (debug()["target"])
    assert s.chase.debug()["target"] == (tracker_fix.x_m, tracker_fix.y_m)
```
Each starts from `finder()` + `until_searching`, places the maybe within 0.5 m of the tank's pose so the check goes straight to its look, and steps with `rt(..., maybes=..., used=...)`.

- [ ] **Step 2: Run** `uv run pytest -q tests/test_drive_roam.py -k "close_look or same_picture or away_from_the_spot or made_up"` — Expected: FAIL (no chase from maybes).
- [ ] **Step 3: Implement** the rules above in `RoamSkill.tick`/`_checking`/`_chase` (`used` is already read in `tick`; pass what `_checking` needs). The look's still-camera rule is unchanged: hits count only once the camera is aimed (`c.aimed`) — a picture taken while it swings is not a close look.
- [ ] **Step 4: Run** `uv run pytest -q` (whole engine suite) — Expected: all pass.
- [ ] **Step 5: Commit** `feat(ai): two pictures of the thing during a close look confirm it (LLD §24 B)`

### Task 4: A lost chase leaves a candidate (D)

**Files:**
- Modify: `ai-engine/src/conqueror_ai/domain/drive/chase.py` (`ChaseSkill.last_seen` property → `XY | None`, the `_target`)
- Modify: `ai-engine/src/conqueror_ai/domain/drive/roam.py` (the `not_found` / `lost` branch of `_chase`)
- Test: `ai-engine/tests/test_drive_roam.py`

**Interfaces:**
- Consumes: `Candidates.add` reopen rule (Task 1), `_maybe_fix` (Task 3).
- Produces: `ChaseSkill.last_seen: tuple[float, float] | None`.

Rule: on `lost` / `not_found`, as today the check's candidate (if any) is ruled out; then, when `chase.last_seen` is not None and inside the play area (same test as maybes), `self.candidates.add(last_seen)` — which reopens the ruled-out one under `REOPEN_MAX`, or makes a new pending one. `_maybe_fix` cleared.

- [ ] **Step 1: Write the failing tests** — replace `test_a_candidate_whose_chase_gives_up_is_ruled_out` (it pinned "nothing left", which §24 D changes; review M1's endless-loop concern is kept by the bound):

```python
def test_a_lost_chase_leaves_a_candidate_at_the_last_seen_spot() -> None:
    # check at (1.2, 0.6), fix there -> chase, then nothing: after the chase gives up, one pending candidate near (1.2, 0.6)
    assert [k.spot for k in s.candidates.pending()] == [(1.2, 0.6)]

def test_a_thing_lost_again_and_again_is_dropped_after_two_reopens() -> None:  # review M1 kept
    # repeat check -> seen -> lost three times: then nothing pending and no further check of that spot
    assert s.candidates.pending() == []

def test_a_chase_lost_outside_the_play_area_leaves_no_candidate() -> None:
    # finder() has area_m 2.0: a fix at 2.5 m from the start, lost: no candidate
    assert s.candidates.pending() == []
```

- [ ] **Step 2: Run** `uv run pytest -q tests/test_drive_roam.py -k "lost"` — Expected: FAIL.
- [ ] **Step 3: Implement** `last_seen` and the branch above.
- [ ] **Step 4: Run** `uv run pytest -q && uv run ruff check . && uv run pyright` — Expected: all pass, 0 errors.
- [ ] **Step 5: Commit** `feat(ai): a chase that loses the thing leaves a candidate at its last seen spot (LLD §24 D)`

### Task 5: As built, replay, docs

**Files:**
- Modify: `docs/lld/ai-engine-lld.md` (§24.7 as built: the constants, test names, rulings), `docs/design-notes.md` (§215 built), `CLAUDE.md` (plans list + plan order row for Plan Z9)

- [ ] **Step 1: Replay the room3 recording** (the decisions differ from the recorded ones — expected; it must run to the end without an exception):
Run: `cd ai-engine && uv run cq-drive --replay ../../rec_room3/word-4-5 > ../../replay_z9.log 2>&1; echo $?; tail -3 ../../replay_z9.log`
Expected: exit 0. If the replay tool needs the session folder rather than a word folder, use `../../rec_room3`.
- [ ] **Step 2: Write §24.7, §215 "built", CLAUDE.md rows** (Plan Z9 built, floor check pending).
- [ ] **Step 3: Full suite** `uv run pytest -q && uv run ruff check . && uv run pyright` — Expected: all pass.
- [ ] **Step 4: Commit** `docs: Plan Z9 as built (AI engine LLD §24.7, design-notes §215)` and push.

Then the final whole-branch review (one opus reviewer), its fixes, and the owner's floor check (§24.6): FIND toy with the first look from ~1 m, FIND a chair — before each: what moves, steps, floor confirmed, camera check.
