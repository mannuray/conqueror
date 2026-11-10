# Plan G2 — the engine's side of the people games: PersonWatch and FACE / ESCAPE / FOLLOW Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** the AI engine knows where the person is (`PersonWatch`), streams it to the bridge during a game, answers whether a backward move is over trusted floor, passes the seeing-test lock in its hello, and runs the three game skills FACE, ESCAPE and FOLLOW behind the Z7 `Skill` interface.

**Architecture:** pure domain pieces (`domain/person.py`: `person_range`, `PersonEst`, `PersonWatch`; `domain/drive/trusted.py`: `TrustedFloor`; `domain/drive/game_skills.py`: the three skills) fed by the existing `Perception` → `World` → `SkillInput` path; the skill server (`app/serve.py`, `adapters/skill_link.py`) gains the game's messages. The person boxes are taken before `person_guard.clear`; the tracker, the memory, the map and `Names` keep refusing people as today.

**Tech Stack:** Python 3.12 (uv, pytest, ruff, pyright strict), numpy; the engine's `Perception`, `World`, `RoomMap`, `CostMap`, `RoutePlanner`, `SkillRunner`, `LeaseDriver`; `tests/fake_bridge.py`.

**Spec:** AI engine LLD §28 (`docs/lld/ai-engine-lld.md`, approved 2026-11-09), bridge LLD R34 / R34.3 / R34.9 (the messages it must match), Spec 10 (`docs/superpowers/specs/2026-11-08-10-people-games-design.md`), design-notes §223-§226. Branch `plan-g2-person-watch` (worktree `scratchpad/plan-g2-person-watch`, from `main` 6e04f51).

## Global Constraints
- **People stay out of everything else:** the person boxes go to `PersonWatch` only; `person_guard.clear`, the thing tracker, `ThingMemory`, the room map, `Names` (`find "person` → `unknown_thing`) are unchanged.
- **The game's person words** (`"baby"`, `"person"`, `"kid"`) resolve only for `face` / `escape` / `follow` and for questions marked `"game":true`; anywhere else they stay `unknown_thing`.
- **Messages (must match bridge LLD R34.3 / R34.9):** hello `"people": true|false`; bridge → engine `{"t":"game","on":bool}`; engine → bridge `{"t":"person","seen":bool,"d_cm":int(−1..1000),"bearing":int(−180..180, + left),"coming":−1|0|1,"sure":0..1,"age_ms":int}` at each used picture and at least every 200 ms while game on; `skill_ask` `{"q":"back_ok","cm":n}` → `skill_answer` `true|false` within 300 ms; `skill_start` `"skill":"face"|"escape"|"follow"` with `"thing"`, escape `"cm"` (20-100), follow `"s"` (1-60) and `"stop_cm"` (optional, default 100, never under 50); `skill_end` `"why":"caught"` ends the word quietly.
- **`PersonWatch` numbers (AI engine LLD §28.2):** the distance from the tank's front edge to the box's bottom-centre floor point; within ±12° of ahead a nearer sonar echo wins; median over 0.3 s; `coming` ±1 when the distance changed ≥ 5 cm over 0.5 s; reported `d − 2σ` with σ = 0.10 m up to 1 m and 0.20 × d beyond (until P0 measures it); a box cut by the picture's bottom edge (within 4 px) reads 0.35 m unless the sonar says otherwise; unseen and last inside 0.50 m: the distance keeps falling at the last closing speed and reads 0.30 m after 1 s unseen; never rising while unseen.
- **Trusted floor (§28.4):** every 10 cm cell the tank's footprint sweeps along the backward path was seen free within the last **5 s** (floor marks are all within 1.5 m of the lens), with no low / high mark since.
- **Game skills' speed:** `Limits` with `v_cap_cms` **25** and the existing `dv_cms_per_s` 10; backing at `v_min_cms` (18) as `curves.py`'s backing.
- **The seeing-test pass:** `config/person-seeing.yaml` (numbers only: date, the detector model's file name, the calibration's date, the marks passed); `people` true only when it exists and names the detector in use and the calibration in use.
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A person box and a thing box overlapping (a child holding a toy): the toy is still dropped by the guard and never chased, the person reaches `PersonWatch` only.
2. Two people in the picture (a parent and the baby): the nearest on the floor is "the person"; a standing parent's box (bottom = feet) ranges correctly.
3. The engine restarting mid-game (the bridge holds; the new engine must not stream as if the game went on: no `person` until the bridge says `game on` again).
4. A camera that freezes: `age_ms` grows with the picture's age, never reset by a resend (the bridge checks it, R34.9 I1).
5. ESCAPE when the person is behind the tank's camera view (bearing beyond ±41°, unseen): it does not back towards where they were last seen.

---

### Task 1: `domain/person.py` - where the person is
**Files:** create `ai-engine/src/conqueror_ai/domain/person.py`; modify `domain/person_test.py` (import `PersonRange`, `person_range`, `pick_person` from `person.py`); test `tests/test_person_watch.py`.

**Interfaces - Produces:** `PersonRange` / `person_range(cam, box) -> PersonRange` / `pick_person(ranges)` (moved, unchanged); `@dataclass(frozen=True) class PersonEst: seen: bool; d_m: float | None; bearing_deg: float; coming: int; sure: float; t_us: int` with `wire(now_us) -> dict` (the `person` message: `d_cm` −1 when None, `age_ms` = now − t_us); `class PersonWatch: update(boxes: list[tuple[float,float,float,float]], cam: CameraModel | None, sonar_m: float | None, t_us: int) -> PersonEst; latest(now_us: int) -> PersonEst | None`.
- [ ] **Failing tests:** a box 1.0 m ahead → `d_m` ≈ 1.0 − 0.20 = 0.80 (2σ, σ 0.10) and bearing 0; 1.5 m → 1.5 − 0.60; the sonar at 0.6 m within 12° beats a camera 0.9 m; outside 12° it does not; a cut box → 0.35 (or the nearer sonar); two boxes → the nearer; `coming` +1 over 0.5 s of 1.0 → 0.8 m, −1 the other way, 0 under 5 cm; seen at 0.45 m then nothing: `latest` falls with the last closing speed and is 0.30 at 1 s unseen, never rising; seen at 1.2 m then nothing: stays 1.2 (unseen, not counting down); `wire()` gives the R34.3 message and `age_ms` from the estimate's own time; `person_test` tests still pass.
- [ ] FAIL → implement → PASS; ruff, pyright; commit `feat(ai): PersonWatch - where the person is, pessimistic (AI engine LLD §28.2)`.

### Task 2: the person boxes into the World; trusted floor
**Files:** `app/perception.py` (the raw `person` boxes kept before `clear`, handed to the World with the picture's camera), `app/world.py` (`World.person: PersonWatch`; `SkillInput.person: PersonEst | None` from `latest`), `domain/drive/skill.py` (`SkillInput.person`), `domain/room_map.py` (`free_us: NDArray[np.int64]` per cell - the last time it was marked free floor or driven over; a low / high hit resets it to 0), create `domain/drive/trusted.py` (`TrustedFloor.ok(room, pose: XYTh, back_m: float, now_us: int, max_age_s=5.0) -> bool`); tests `tests/test_person_world.py`, `tests/test_trusted.py`.
- [ ] **Failing tests:** a picture with a person box and a toy box inside it: the toy never reaches the tracker (as today) and `world.person.latest` is set (Review Focus 1); no person box → `SkillInput.person` stays the last estimate, unseen; `free_us` set by floor marks and by the driven footprint, reset by a mark; `TrustedFloor.ok` true for a 30 cm straight back over cells freed 2 s ago, false when one cell is 6 s old, never seen, or marked low.
- [ ] FAIL → implement → PASS; the whole suite (Perception / World users unchanged otherwise); commit `feat(ai): the person boxes to PersonWatch, the room map's free-floor times, TrustedFloor (AI engine LLD §28.1, §28.4)`.

### Task 3: FACE, ESCAPE, FOLLOW
**Files:** create `domain/drive/game_skills.py` (`FaceSkill`, `EscapeSkill`, `FollowSkill`, each a Z7 `Skill`); `domain/drive/skill.py` (`Result.CAUGHT` - not an error); tests `tests/test_game_skills.py` (the engine's `sim.py` / sim test helpers for driving).
- **`FaceSkill(lim)`:** ≤ 1 s. |bearing| ≤ 35°: the camera pans to it (`SkillOut.cam`), no motion; beyond: turns on the spot towards it (`Command(0, ±w)`, the slowest smooth spin) until within 10°. `done` facing; `lost` after 1 s unseen.
- **`EscapeSkill(lim, back_m, trusted: TrustedFloor)`:** the person ahead (within ±41°): `TrustedFloor.ok` for `back_m` behind → back (`Command(−v_min, 0)`) until the odometry moved `back_m`, `done`; not trusted and the person ≥ 0.70 m → turn ~180° (look), then forward `back_m` over the cost map's allowed floor if the route is clear, then turn back to face, `done`; else stay, `no_way` (S11). Person unseen / beyond ±41°: never backs - `no_way` (Review Focus 5).
- **`FollowSkill(lim, seconds, stop_m)`:** `stop_m = max(0.50, stop_m)`; the goal the stop distance short of the person on the tank's side (never the person's spot); a route there with `RoutePlanner` at ≤ 25 cm/s; stops (and keeps facing) inside `stop_m`; `done` at `seconds`; `lost` after 1 s unseen.
- [ ] **Failing tests (sim):** FACE at 20° pans and does not drive; at 60° turns left until within 10°; unseen 1 s → `lost`. ESCAPE with trusted floor behind backs `back_m` (odometry) and never turns; untrusted and person at 0.9 m: turns, drives, turns back; untrusted and 0.5 m: `no_way`, never moves; unseen: `no_way`. FOLLOW from 2 m with stop 1.0: drives, stops by 1.0 m, never a goal inside 0.5 m even with `stop_m` 0.3; `lost` when unseen; every command ≤ 25 cm/s.
- [ ] FAIL → implement → PASS; commit `feat(ai): the game skills FACE, ESCAPE and FOLLOW (AI engine LLD §28.3)`.

### Task 4: the skill server's game messages
**Files:** `adapters/skill_link.py` (hello `people`; parse `game`, the new `skill_start` fields, `skill_ask` `back_ok` / `"game"`; send `person`), `app/serve.py` (`SeeingPass`, the person stream task: each used picture and ≥ 5 Hz only between `game on` and `game off` - Review Focus 3; `face` / `escape` / `follow` words through `SkillRunner` with the game `Limits`; `back_ok` from `TrustedFloor`; the person words; `why: caught` ends the word as `CAUGHT`, no `skill_failed`), create `app/seeing_pass.py` (`SeeingPass(path, detector_name, calibration_date).passed() -> bool`); tests `tests/test_serve_game.py` (with `FakeBridge`), `tests/test_seeing_pass.py`.
- [ ] **Failing tests:** hello `people` false with no pass file, true with a matching one, false when it names another detector; `game on` → `person` messages at ≥ 5 Hz with `age_ms` from the picture's time (a frozen picture's age grows - Review Focus 4), none before `game on` or after `game off`; `skill_ask back_ok cm 30` answered true / false from the map within 300 ms; `skill_start face "baby` runs FaceSkill, `find "baby` stays `unknown_thing`; `howfar "baby` with `"game":true` answers the front-edge distance in cm, without it −1; `skill_end caught` ends a FOLLOW quietly.
- [ ] FAIL → implement → PASS; the whole suite; commit `feat(ai): the skill server plays - people, the person stream, back_ok, FACE / ESCAPE / FOLLOW words (AI engine LLD §28, bridge LLD R34.3)`.

### Task 5: the seeing test writes its pass
**Files:** `app/person_test.py` (`score` writes `config/person-seeing.yaml` when the detector the engine uses passes every mark; never on a fail), test `tests/test_person_test_app.py`.
- [ ] **Failing test:** a recorded folder that passes → the file with the date, the model's file name, the calibration's date, the marks; a failing one → no file (an old one left untouched? **removed**, so a worse result locks the game again).
- [ ] FAIL → implement → PASS; commit `feat(ai): a passed seeing test unlocks the game (AI engine LLD §28.6)`.

### Task 6: docs as built, the review
- [ ] AI engine LLD §28.9 as built, design-notes §227, CLAUDE.md plans row; commit, push. Opus final review of the branch (the Review Focus above); fixes with a failing test first.
- [ ] With the owner (after G1's stand checks and P0): `cq-drive serve` from this branch with the stand-in program `REPEAT 30 [ IF CAUGHT? [ WAIT 3 ] FACE "person WAIT 1 ]` - an adult in front, the tank on the floor, STOP in hand.
