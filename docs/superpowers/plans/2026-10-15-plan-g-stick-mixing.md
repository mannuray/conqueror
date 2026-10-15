# Plan G — Stick steering by angle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (owner: native) task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** a 45° stick push makes a forward arc, spin only near fully sideways; IR unchanged.
**Architecture:** new angle mix for the driver's steering in `mixLite` (Uno Lite, integers) and `mix` (full, floats); the `w_steer == 0` path stays the old arcade mix.
**Spec:** spec 02 §A4.1; UX hub-ux.md "Drive stick"; LLD Uno Lite §16 + Uno LLD decision note.

## Global Constraints
- IR track outputs identical to today (pure axes ±s), heading-hold path identical (w_steer 0).
- Uno: whole numbers only in the Lite; flash ≤ 30,720 B, RAM ≤ 1,536 B; no heap/virtual/delay.
- Safety untouched: SafetySupervisor, obstacle brake, caps, ramps run after the mix as today.
- Commits: Conventional Commits + `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus
1. |v| + |w| = 0 never reaches the angle path (w_steer ≠ 0 guarantees it). 2. Signs in reverse (both tracks backwards, car-style unchanged). 3. IR ±80 on each axis = old values. 4. Integer overflow in m·r (≤ 100 × 1000). 5. Full firmware pivot_fallback still applied after the new mix.

### Task 1: Uno Lite angle mix (host-tested)
- [ ] Failing tests in `test/test_drive_mix_lite` (names in Lite LLD §16) → run, FAIL.
- [ ] Implement `angleMixLite` + branch in `mixLite`; run the whole native suite; `pio run -e uno` size check.
- [ ] Commit `feat(uno): stick steering by angle - forward arc at 45°, spin only near sideways (IR unchanged)`.

### Task 2: Full firmware `mix()` (host-tested)
- [ ] Failing tests in `test/test_mixer` (decision note) → FAIL; implement; native suite; `pio run -e mega`.
- [ ] Commit `feat(uno): full firmware stick mixing by angle`.

### Task 3: Tank check (owner present)
- [ ] Flash the Uno over USB (S1 upload, then back; unplug USB). Stand: 45° → both tracks forward, inner slower; 90° → spin; IR ▲◀▶ as before.
- [ ] Floor: arcs feel; spin speed with MAX 60 %; decide whether a Uno spin cap is needed. Record in design-notes §47.
