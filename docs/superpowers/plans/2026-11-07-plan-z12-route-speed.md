# Plan Z12 — a quicker route planner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `RoutePlanner` about 2× quicker (the test: at least 1.5×, owner 2026-11-08) with exactly today's routes, costs and nearest cells.

**Architecture:** the search loops in `domain/drive/route.py` run on flat lists over a grid padded with closed cells, with neighbour offsets worked out once; today's planner stays as a test-only reference to compare against.

**Tech Stack:** Python 3.12 (uv, pytest, ruff, pyright strict), numpy.

**Spec:** AI engine LLD §27, design-notes §222. Branch `plan-z12-route` (worktree `scratchpad/plan-z12-route`, from `main` 0e3cf46).

## Global Constraints
- Results identical to today's planner (same lists of points, same cost arrays, same cells), not merely equally cheap.
- Signatures of `route`, `nearest`, `costs`, `grid_of`, `grid_centre`, `aim_point` unchanged; no caller changes.
- No new dependency. `test_route_is_quick` (20 ms) unchanged.
- Commits: Conventional Commits ending `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. Ties: equal-cost cells must pop in today's order (padded numbering monotonic in (i, j); heap tuples unchanged).
2. The start cell in a closed cell (the tank inside a mark) still opens, only for that search.
3. `near_m > 0` ends within reach exactly as today (the inline `arrived`).
4. A goal off the map or on a closed cell without `goal_ok` still gives None.
5. Grids of other sizes (`cost.n` not a multiple of k) and a grid per cost map (the cache) still right.

---

### Task 1: the reference and the comparison tests
**Files:** create `ai-engine/tests/route_reference.py` (today's `RoutePlanner` search code, verbatim, as `ReferencePlanner`, reusing `_Grid`'s factor); `ai-engine/tests/test_drive_route.py`.
- [ ] Copy today's `route` / `nearest` / `costs` into `ReferencePlanner` (pyright strict clean).
- [ ] `test_the_quick_planner_gives_todays_routes` (30 seeded maps, the cases of LLD §27.4) and `test_the_quick_planner_is_at_least_twice_as_quick` (the same routes, ratio ≥ 1.5 - owner 2026-11-08; designed as ≥ 2).
- [ ] Run: the comparison passes (both are today's code), the speed test FAILS (ratio ~1).
- [ ] Commit `test(ai): today's route planner as a reference; the quick planner must match it (AI engine LLD §27.4)`.

### Task 2: the quick search
**Files:** `ai-engine/src/conqueror_ai/domain/drive/route.py`.
- [ ] `_Grid.padded`, `nbrs`, `pad`, `unpad`, `search_from` (LLD §27.2-§27.3); `route`, `nearest`, `costs` on flat lists.
- [ ] Run `tests/test_drive_route.py`: all pass, ratio printed; then the whole suite, ruff, pyright.
- [ ] Commit `perf(ai): the route planner searches flat padded lists - the same routes, at least 2x quicker (AI engine LLD §27)`.

### Task 3: docs as built
- [ ] LLD §27.5 as built (the measured times and ratio), design-notes §222 "Built", CLAUDE.md plans row; commit, push. Opus final review.
