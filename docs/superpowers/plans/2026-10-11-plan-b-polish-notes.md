# Plan B — plan polish notes (2026-10-11)

The Plan B plan was checked and polished before execution (owner: "polish the plan, don't execute it"). Nothing was executed; no firmware exists yet.
Result: 15 tasks, **131 host tests** (all compiled and passing on the host from the plan text), smoke script ruff + format + pyright strict clean. AVR drivers and `main.cpp` were not compiled (no avr-gcc on the Mac) — Task 1 Step 2a is the first AVR gate.

## How
Pre-flight scans (Tasks 1–5, 6–10, 11–15) → controller rulings → three fixers by range → final cross-task consistency check.
Commits: `8ae1a1c` (parser replay budget), `34df3c6`..`8d25f3f` (Tasks 1–5), `42f92d3`..`9a8fdba` (Tasks 6–10), `3034b94`..`f175293` (Tasks 11–15 + Plan C sync), `49c4b67`..`a38562c` (final consistency).

## Rulings (decisions taken while polishing)
- R1 Degraded caps (L4623, L4849, L6743, L8085): follow LLD §9.5/§9.16 — add `degraded_cap_pct` to TargetLimiter `Limits` (cap = min(envelope, degraded); sets capped + FastFlagBit::kCapped); Task 11 passes 50 when ultrasonic faulted, 30 when IMU faulted (min of both). Tests: Task 6 `test_fault_range_does_not_block` (0xFFFE explicit), Task 8 `test_degraded_caps`.
- R2 Sync gate + idle release (L4784, L4519): Task 6 owns both, per LLD §9.4/§9.18 — onDrive (and other network motion) returns BUSY when !link.synced() (IR still allowed); Arbitrator releases Network → None after 1 s idle with primitives idle. Tests for both.
- R3 Stall (L5412/L5426): follow LLD §9.6 — driven above deadband 0.5 s with no edges (gyro shows motion → encoder fault; else stall); stall clears on direction change or after 1 s; SafetySupervisor cuts the stalled track's output. Tests updated/added.
- R4 Integrator freeze (L7011, L8101): keep output-saturation anti-windup AND add `bool capped` OR-ed into the freeze; Task 11 passes Limited.capped. Keep Review Focus 3 test.
- R5 Mixer (L6720): follow LLD §3.4 — turning priority (scale preserving the difference) + pivot fallback using setting `pivot_fallback` (inner target below keep threshold → 0); add `keep_pct`/`pivot_fallback` to MixerConfig + tests.
- R6 TurnProfile min rate (L7464): option (a) — startTurn rejects with kInvalidCommand when computed max_rate < kMinRateDps; test.
- R7 MOVE without IMU (L7686, L8070): MOVE (and heading-hold TURN-to) rejected with kUnavailable when !imu_ok; Task 11 primitives also use the imuUsable() fallback; test.
- R8 LINK_RESTORED on first contact (L1785): add m_ever_alive; first contact after boot does NOT emit LINK_RESTORED; update Expected (L1982) and tests.
- R9 Settings cross-field (L2390): Settings::set rejects with kSettingOutOfRange when the new value breaks pan_min ≤ pan_max, tilt_min ≤ tilt_max, obstacle_slow > obstacle_stop, batt_crit < batt_low against current values; tests. (Hub must order writes.)
- R10 Attitude rate (L3382): follow LLD §9.13 — gyro integration 200 Hz, accel correction (atan2/sqrt) every 4th call (50 Hz); split update into integrateGyro + correctAccel; tests adjusted.
- R11 I2C recovery (L3251): follow LLD §9.13 — recoverBus() (9 SCL clocks + STOP + Wire.begin) on wire timeout, and re-init attempt every 1 s while !imu_ok (non-blocking budget respected).
- R12 Battery smoothing (L3724): follow LLD — 4-sample average, one sample per tick.
- R13 Floor procedures statefulness (L8256): apply the scan's suggested fix (make each smoke run self-contained / re-send needed state).
- R14 WS2812 timing (L9524): add `rjmp .+0` after dec as suggested; update cycle comments and the interrupts-off figure; hardware/scope check goes on the hardware checklist.
- R15 Smoke tools typing (L10483): yes, CLAUDE.md strict rule applies — add pyright to tools/smoke dev group, strict mode with reportMissingTypeStubs=false, `uv run pyright` in the steps; fix the notes list-vs-tuple error.
- R16 Reset reason (L534–562, L553): trust r2 only when plausible ((r2 & ~0x0F)==0 && r2!=0), else kUnknown; relax Step 9 Expected for stock Optiboot.
- All CLEAR findings: apply as suggested.
- Every deviation from the LLD in the CURRENT plan that these rulings keep (none beyond R4's extra anti-windup) → note in the LLD decision notes.
- R17 Ultrasonic fault semantics (Task 5, found while polishing): follow LLD §9.16 (fault after 3 consecutive *failed measurements*, no re-trigger while echo is high, 25 ms cutoff) — BUT a timeout with no echo because nothing is in range must stay "nothing seen" (999/open), not a fault; check LLD §9.16's exact definition of failure and implement that distinction with tests. Owner of edit: T6-10 fixer (edits Task 5 text too).
- R18 Plan C's "exact copy" of tools/smoke/pyproject.toml: update Plan C to match the polished Plan B file (final pass).
- Parser replay budget: `feed()` only when `!pending()`, `next(budget)`, `commsPoll` 64 steps per pass ≈ 0.67 ms worst case.
- IR takeover cancels motion in the input task (no teleop zeroing in the control task).
- Deadman trigger stays 300 ms (network Teleop only); with the closed-loop pipeline the tank is fully stopped ≈ 420 ms after the last command, including the SoftStop ramp.
- `markIntentionalReset` removed (no software reboot path in v1) — **pending owner confirmation**; LLD §9.13 overridden by a dated decision note.
- New `app/ui_api.h` for LED/SOUND/TONE/AUDIO_MODE handlers (same pattern as `camera_api.h`, `motion_api.h`).

## Hardware steps
Executing agents never flash the Uno or run motor tests; hardware steps are run with the owner present, tank on a stand (CLAUDE.md safety rules).
