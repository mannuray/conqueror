# Plan P0 — can the tank see a crawling person? (Spec 10 phase 0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** measure, with an adult crawling at taped marks, whether the engine sees and ranges a person on the floor well enough for Spec 10's 30 cm keep-out - against the owner's bar - before any game code exists.

**Architecture:** one measurement command `cq-person-test` in the engine: `record` guides the owner mark by mark and records pictures + telemetry per mark (nothing drives; only the camera servo moves, to the two tilts); `score` replays a recording through the detectors **without** the thing tracker and memory, turns each person box into a distance from the tank's front edge and a direction, and prints each mark against the bar. Pure scoring functions, unit-tested.

**Tech Stack:** Python 3.12 (uv, pytest, ruff, pyright strict); the engine's `BridgeLink`, `MjpegSource`, recording (`cq-record`'s format), `CameraModel`, YOLO26n (COCO `person`) and YOLO-World.

**Spec:** `docs/superpowers/specs/2026-11-08-10-people-games-design.md` §5 phase 0 (the bar, owner 2026-11-08), design-notes §223. Branch `plan-p0-person` (worktree `scratchpad/plan-p0-person`, from `main` b3d7e24).

## Global Constraints
- **Nothing drives.** The tank stays on its marks; the only motion is the camera servo going to the two tilts (`cam_set`), said before each move.
- **The bar (owner):** at 0.5-2 m seen in ≥ 8 of 10 pictures (still) and ≥ 8 of 10 (crawling); the distance within **±10 cm inside 1 m, ±20 % beyond** (in ≥ 8 of 10 detections); the direction within **5°**; inside 50 cm (0.3 and 0.5 m straight ahead) the **sonar** reads the person within ±10 cm. 0.3 m is reported, not scored for the camera (the sonar's case).
- **Distances** are from the tank's **front edge** (the sonar's face: `Limits.body_front_m` 0.13 m ahead of the centre, 4.5 cm ahead of the lens) to the person's **nearest point on the floor** (their hands), as the owner's tape. The engine's distance: the box's bottom centre through `CameraModel.pixel_to_floor`, minus the front edge.
- **Marks:** 0.3, 0.5, 1.0, 1.5, 2.0 m; straight ahead and 30° right; still (10 pictures) and crawling slowly across the mark (4 s); at the driving tilt (-15°) and level (0°). 40 marks; the owner can skip any.
- **Detectors:** YOLO26n `person` (score ≥ 0.25) and YOLO-World asked `person`, `crawling person`, `baby` (≥ the engine's maybe score); each scored separately, the best reported.
- **Privacy:** recordings and pictures of the person stay on this machine (`ai-engine/recordings/` is git-ignored); nothing with a person is committed; the report is numbers only.
- The engine's person rules unchanged (this tool reads detections directly; it never feeds the tracker, memory or map).
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. A picture with two person boxes (a reflection, the owner's legs and a shadow): the nearest-on-the-floor one is the person at the mark; a box whose bottom is above the horizon gives no distance (counted as not ranged).
2. A box cut by the picture's bottom edge (close up): its bottom is the picture's edge, not the person's hands - reported as "cut", not as a distance.
3. Pictures skipped by the engine's gate (camera moving after `cam_set`) are not scored; the first 0.5 s after a tilt change is dropped.
4. The tape is to the front edge, the camera's floor point is from the lens: the 4.5 cm offset applied once.
5. 30° right: the direction error uses the bearing from the lens, the tape bearing from the tank's centre - the small parallax handled (or the bar applied with it stated).

---

### Task 1: scoring (pure)
**Files:** create `ai-engine/src/conqueror_ai/domain/person_test.py`; test `ai-engine/tests/test_person_test.py`.
- `person_range(cam: CameraModel, box: tuple[float, float, float, float], front_m: float) -> PersonRange` - distance from the front edge (m) and bearing (deg, + left), or a reason: `"cut"` (bottom on the picture's edge), `"sky"` (bottom above the horizon).
- `pick_person(ranges: list[PersonRange]) -> PersonRange | None` - the nearest ranged one.
- `score_mark(mark: Mark, per_picture: list[PersonRange | None], sonar_mm: list[int | None]) -> MarkScore` - seen n / N, ranged within the bar n / seen, direction within 5° n / seen, the sonar within ±10 cm (marks ≤ 0.5 m ahead), pass / fail per the bar.
- `verdict(scores: list[MarkScore]) -> str` - the table and the overall pass / fail per detector.
- [ ] Failing tests: a synthetic camera (the test helpers of `test_camera.py`) with a box whose bottom centre is the floor point 1.0 m ahead of the front edge -> 1.00 m ±1 cm, bearing 0; 30° right -> -30° ±1°; a box cut by the bottom edge -> `"cut"`; above the horizon -> `"sky"`; two boxes -> the nearer; `score_mark` at 1.0 m: 9 of 10 seen, 8 within 10 cm -> pass; 7 of 10 -> fail; at 1.5 m the ±20 % rule; sonar 0.5 m ahead within 10 cm -> pass.
- [ ] FAIL; implement; PASS; ruff, pyright.
- [ ] Commit `feat(ai): score a crawling person's distance against the tape (Spec 10 phase 0)`.

### Task 2: `cq-person-test record` and `score`
**Files:** create `ai-engine/src/conqueror_ai/app/person_test.py`; `ai-engine/pyproject.toml` (`cq-person-test`); test `ai-engine/tests/test_person_test_app.py` (with `tests.fake_bridge.FakeBridge`).
- `record DIR`: connects (the tank's stream + telemetry); per mark prints "Camera to -15° (only the camera moves). Crawl to 1.0 m straight ahead, hands on the tape, stay still - Enter (s to skip)"; records 10 pictures (still) or 4 s (crawling) with telemetry into `DIR/<mark-id>/` in `cq-record`'s format plus `mark.json` (tape m, angle, still / crawling, tilt). Ctrl-C keeps what is recorded.
- `score DIR`: per mark replays the pictures through both detectors (no tracker / memory / map), the camera from the recorded pan / tilt and calibration, drops pictures within 0.5 s of a tilt change, runs `score_mark`, prints `verdict` and writes `DIR/report.txt` (numbers only).
- [ ] Failing tests: `record` against `FakeBridge` with scripted Enter / s writes two marks and skips one; `score` on a recorded synthetic folder (a person-sized box drawn at a known floor point, the detector faked) prints the expected pass line.
- [ ] FAIL; implement; PASS; whole suite, ruff, pyright.
- [ ] Commit `feat(ai): cq-person-test - record a crawling person at taped marks and score it (Spec 10 phase 0)`.

### Task 3: the test with the owner, then the answer
- [ ] Tape on the floor: the tank's front edge at 0; marks at 0.3 / 0.5 / 1 / 1.5 / 2 m ahead and on a 30° line right. The owner crawls; I say before each camera move. `cq-person-test record recordings/p0-<date>`, then `score`.
- [ ] Docs: design-notes "Spec 10 phase 0 - the result" (the table, pass / fail against the bar, what failed and why); Spec 10 §5 phase 0 marked done with the answer; CLAUDE.md row. If it fails: options back to the owner before anything else (a camera change, the tilt for the game, the sonar as the near sense).
- [ ] Opus final review of the code (Tasks 1-2).
