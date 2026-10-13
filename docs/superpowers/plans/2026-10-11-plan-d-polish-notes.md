# Plan D — plan polish notes (2026-10-13)

Plan D (bench tool) was polished natively (no subagents, owner request): every task's code was extracted from the plan, built in a scratch workspace against the real Plan A `conqueror_protocol` (protocol v2), and run through pytest, ruff, `ruff format --check` and pyright strict; fixes were written back into the plan. Final check: the whole tool rebuilt **from the plan text alone** → **92 tests pass**, all gates clean. Nothing was executed in the repo.

## Defects found and fixed
- **No Python pin** → uv picked 3.14; added `tools/bench/.python-version` = 3.12 (Task 1).
- **Odometry baseline taken after motion started** (`set_origin` used the first sample after the next command) → distance read ~2 % short, spin ~1 %. Now waits for a fresh stationary sample; runner also records ~3 frames after the last step (Task 9).
- **Simulator ignored obstacles placed after start** → `place_obstacle()`; the safety test places the box at its prompt (Tasks 2, 9).
- **Speed-hold test expected the full spread**; the metric (and the roadmap's "±5 %") is deviation from the mean → test corrected (Task 7).
- **CLI imported the dashboard before it existed** → `drive`/`dash` commands moved to Task 15 (Tasks 14, 15).
- **Console input not focused** → typed commands went nowhere; `AUTO_FOCUS` (Task 15).
- **`_on_update` overrode Textual's internal method** → renamed `_on_model` (Task 15).
- Race in the envelope handshake test; private `_subs` access replaced by `TankModel.unsubscribe()`; event-based waits instead of sleep loops; read-only `Calculator` protocol; matplotlib/Textual typing boundaries (`Any` / scoped pyright relaxation in `app.py` only); lint/format throughout.

## Additions from owner decisions
- "Bridge busy" when the bridge closes the socket during the handshake (Task 3).
- Safety Override: `CommandApi.safety_override()`, simulator rules (human only, BUSY before sync, ENVELOPE_LOCKED under child play), console `override on|off`, dashboard shows `⚠ OVERRIDE` / `⛔ LOCKED` (Tasks 2, 5, 14, 15).
- Simulated WiFi stall 0.6 s (bridge drops a silent bench after 1 s) (Task 9).
- Task 16 acceptance: Linux CH340 port, reset reason UNKNOWN on Optiboot 4.4, pin-pairing check via the probe, Override / self-test lock / 45° tilt checks on the stand.
