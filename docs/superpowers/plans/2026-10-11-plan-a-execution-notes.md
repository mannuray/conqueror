# Plan A — execution notes (2026-10-11)

Plan A (protocol generator) was executed with subagent-driven development on branch `plan-a` and merged to `main`.
Result: 153 tests, ruff + pyright strict clean, `protogen check` clean. This page records the decisions taken during execution and the review items left for later.

## Decisions taken during execution
| # | Decision | Why |
|---|---|---|
| 1 | 15 plan defects fixed in the plan text before Task 1 (vector value overflow, unquoted `Busy, try again`, `cli.py` importing `diff` before Task 8, lint/pyright issues, TONE count check, `\|` escaping in docs, test counts) | Briefs are extracted from the plan; fixing at source keeps every task consistent |
| 2 | Pydantic models use `extra="forbid"` | YAML typos must fail loudly |
| 3 | `naming.py` moved from Task 4 to Task 3 | Validator reuses `pascal()` instead of duplicating it |
| 4 | `protogen/py.typed` added; `protocol/.python-version` = 3.12 | pyright strict needs the marker; uv had picked 3.14 |
| 5 | **Frame parser resync** (deviation from the plan and the Uno LLD): an out-of-range length byte that is `0xAA` starts a new frame; after a CRC mismatch the bytes after the false start are rescanned and replayed | Noise or a cut-off frame must not cost the next good frame. Recorded in `protocol-generator-lld.md` and `uno-firmware-lld.md`; Plan B parser rewritten to match |
| 6 | No inter-byte timeout in the Python parser | 100 ms heartbeats keep the stream moving; worst case one frame of delay |
| 7 | `protogen diff` extended beyond the plan: error codes, enums, settings, field scale/enum/sentinel, frame ack flag; exit 2 when the ref can't be read | Codes must never be reused; these change wire meaning |
| 8 | Field `range`/`unit`/`doc` changes are not breaking | They don't change bytes. Nothing enforces `range` at runtime, so consumers (Uno domain code) must clamp |
| 9 | `wire.encode` raises `ValueError` for out-of-range values | Plan D's console catches `ValueError` |
| 10 | `docs/dev-setup.md` has the `git config core.hooksPath .githooks` step | Fresh clones (Linux machine) otherwise run without the staleness hook |

## Carried into Plan B
- Uno parser must handle resync with a **bounded per-pass replay budget** (final review: a crafted stream could cost ~9 ms of each 10 ms tick). Fixed in the Plan B plan before Plan B execution.
- First build gate: compile `cq_protocol.h` with avr-g++ `-std=gnu++17` (not verified on the Mac — no AVR toolchain at Plan A time).
- `decode()` returning `false` leaves the message contents unspecified — never use it.
- Parser counters `bad_len`/`bad_crc` count rejection events (a replay can count again).

## Deferred review items (non-blocking)
- Validator: no injected-defect tests for several branches (duplicate frame/field/error/setting, `ack_status` rules, unknown golden frame, `count_field` must be u8, exact 48-byte boundary); `test_error_code_outside_domain` doesn't assert "outside"; no fixed-point scale rule; enum values not checked to fit the wire type; `count <= 0` not rejected; more than one struct array per frame not rejected.
- Model: `Field` doesn't reject nonsensical combinations (`items` + `type`/`count`); bare `assert` used for narrowing; `WIRE_SIZE` keyed by `str`.
- Reference encoder (test-only): no count-vs-array or ≤ 48 checks; `build_frame` masks `seq`; extremes never emit u32 = 0 or seq = 0xFF.
- No Uno→ESP32 telemetry golden frame (ACK golden covers that direction).
- `emit_python` pastes YAML text into `"..."` without escaping; `emit_docs` doesn't escape newlines.
- C++ tests: vector count not asserted; TONE test lacks `-pedantic` and the `len == 0` / short-length cases.
- `protogen diff`: events and meta not compared; traceback (not exit 2) if `git` is missing or the old defs are invalid; tests hardcode the `AUTH` domain.
- Hook checks the working tree, not the staged index (revisit when CI exists).
- `wire._pack` docstring says "wrong-shape"; a wrong-shape item still raises `TypeError`.
- `esp32-firmware-lld.md` line ~214 says the parser is generated C++ — update at the next ESP32 LLD revision.
- `u32` setting with max > INT32_MAX would fail to compile in `SettingDescriptor` (loud, not silent).
