# Conqueror Platform — Coding Standards

Status: PROPOSED for review · Date: 2026-10-10
Applies to: `firmware/uno`, `firmware/esp32`, `hub`, `ai-engine`, `protocol`.

---

## 1. Common rules (all codebases)

### 1.1 SOLID, applied per platform
| Principle | Rule everywhere |
|---|---|
| **S** | One module/class = one reason to change. Files stay small and focused. |
| **O** | Extend by **adding** (new table row, new implementation, new handler), not by editing core logic. Closed sets of variants are allowed at one marked place (`VARIANT POINT`). |
| **L** | Every implementation of a contract honours it fully: units, ranges, timing, failure value. Contracts are written on the interface/port. |
| **I** | Small, role-specific interfaces/ports (1–5 members). No "god" interfaces. |
| **D** | Core/domain code depends on **ports** (abstractions); adapters/drivers implement them; wiring happens only in a **composition root**. No hidden globals/singletons outside it. |

How polymorphism is realised differs by platform (by design):
| Platform | Dependency inversion mechanism |
|---|---|
| Uno | port headers + **link-time** implementation selection; **no `virtual`** |
| ESP32 | same as Uno; `virtual` only when ≥ 2 implementations are active at runtime |
| Hub (TS) | interfaces + constructor injection |
| AI engine (Python) | `typing.Protocol` ports + constructor injection; runtime swapping is a core feature (LLM/perception providers) |

### 1.2 Layering (hexagonal / ports and adapters)
```
composition root  →  application (use cases, orchestration)  →  domain (pure logic)
                                     ↓ ports (interfaces)
                         adapters / drivers (hardware, network, DB, models)
```
- **Domain is pure:** no I/O, no framework, no hardware, no clock — time and I/O come through ports.
- Dependencies point **inward** only.

### 1.3 Shared conventions
- **Protocol single source:** all messages are defined in `protocol/` and generated into each language. **Generated code is never hand-edited.**
- **Units are explicit:** strong types or unit suffixes (`dist_mm`, `yaw_ddeg`, `timeout_ms`, `speed_pct`). No unit-less magic numbers; named constants only.
- **Fail safe:** on doubt or unexpected state, the safe action wins (stop motors, reject command, hold position).
- **Safety by structure:** the paths that can move the tank are few, explicit and reviewed (Uno: only `SafetySupervisor` holds the motor output).
- **Comments explain *why*,** not what. Each public type documents its contract (units, ranges, timing, thread/ISR safety).
- **Function size:** aim ≤ ~40 lines, cyclomatic complexity ≤ 10, ≤ 4 parameters (else a parameter struct/object).
- **Errors are never silently swallowed.** Every caught error is handled, converted, or logged with context.
- **Commits:** Conventional Commits (`feat(uno): …`, `fix(hub): …`, `docs: …`), one logical change per commit.
- **Formatting and linting are automatic** and must pass before commit (configs live in the repo).

---

## 2. Firmware — Arduino Uno (`firmware/uno`)

### 2.1 Structure
```
src/main.cpp        composition root: all objects static, wiring, scheduler start
src/app/            scheduler, comms dispatcher, telemetry builder
src/domain/         control, safety, arbitration, primitives, UI catalogues — NO Arduino.h
src/ports/          port headers (yaw_source.h, motor_output.h, encoder.h, clock.h, …)
src/drivers/        port implementations for the Uno (Arduino.h allowed only here)
src/generated/      protocol codec (generated)
test-fakes/         alternative port implementations for host builds (link-time swap)
```

### 2.2 Polymorphism
- **No `virtual` functions.**
- Closed sets of variants (speed controller modes, primitives) = **enum + switch** in one place marked `// VARIANT POINT`.
- Extensibility through **flash tables** (`PROGMEM`): sounds, LED patterns, IR codes, settings descriptors.
- Templates only for small generic utilities (`Pid<T>`, `RingBuffer<T,N>`, `Filter<T>`).

### 2.3 Memory and real time
- **No heap:** no `new`, `malloc`, Arduino `String`, STL containers. Static allocation, fixed capacities, `constexpr` sizes.
- **No blocking:** no `delay()`, `pulseIn()`, busy-waits > 50 µs. Every task is a non-blocking `update()` called by the scheduler.
- **ISRs:** < 10 µs, capture only (timestamps/counters); shared data `volatile`; multi-byte reads in `ATOMIC_BLOCK`.
- **Time only via the clock port;** wrap-safe comparisons `(now - start) >= interval`.
- **No exceptions, no RTTI.** Results are explicit enums (`enum class Status`).
- Stack depth kept shallow; no recursion. RAM budget tracked in CI/build output; flash/RAM report on every build.

### 2.4 Style and tooling
- C++17 (`-std=gnu++17`), `-Wall -Wextra -Werror`, `-fno-exceptions -fno-rtti`.
- `clang-format` (repo config), `.h`/`.cpp` pairs, `#pragma once`.
- Naming: `PascalCase` types, `camelCase` functions, `snake_case_unit` variables, `kConstant`, `m_member`, ports named for their role (`MotorOutput`, `YawSource`).
- `clang-tidy` + `cppcheck`, curated MISRA-C++-inspired rule set (no full MISRA compliance).
- `enum class` over plain enums; `static_cast` only; no implicit narrowing; `const` by default.

---

## 3. Firmware — ESP32 (`firmware/esp32`)
Same as §2 except:
- `virtual` allowed only where ≥ 2 implementations are active at runtime.
- **FreeRTOS:** tasks communicate via **queues/messages**; shared state has one owner or a mutex; no blocking calls inside high-priority tasks; every task feeds the task watchdog.
- **Allocation at startup only;** none in hot paths (video, control, body link). No Arduino `String` in hot paths; JSON via fixed-capacity documents.
- Domain (arbitration, navigation, auth rules) is pure C++, host-buildable.
- Secrets (key hashes, passwords) never logged.

---

## 4. Hub — Node.js + TypeScript (`hub`)

### 4.1 Language and tooling
- TypeScript `strict: true`, `noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`. **No `any`** (use `unknown` + narrowing).
- ESLint (typescript-eslint strict + `no-floating-promises`) + Prettier. ESM modules.
- Layers: `domain/` (pure), `application/` (use cases), `adapters/` (tank WS client, HTTP, SQLite, ffmpeg, mDNS), `ui/` (React), `main.ts` (composition root). Constructor injection; no service locator.

### 4.2 API shape (hub REST + WebSocket for browsers and the AI engine)
- **Versioned:** `/api/v1/...`. Resource-oriented nouns: `/api/v1/tanks/{tankId}/missions/{missionId}`.
- JSON, `camelCase` fields, ISO-8601 UTC timestamps, explicit units in names (`distanceCm`).
- **Validation at every boundary** with schemas generated from `protocol/` (zod). Unvalidated data never reaches the domain.
- Lists: cursor pagination (`?cursor=&limit=`), stable ordering.
- Commands that change state accept an **idempotency key** (`Idempotency-Key` header / `id` field).
- WebSocket messages reuse the tank protocol envelope (`t`, `id`, `seq`) so browsers, AI engine and tank speak one dialect.
- OpenAPI document generated from the route schemas and served at `/api/v1/openapi.json`.

### 4.3 Error handling
- **Custom error class hierarchy** rooted at `AppError` (`code`, `httpStatus`, safe `details`, internal `cause`); expected failures throw subclasses (`ValidationError`, `NotFoundError`, `ConflictError`, `TankOfflineError`, …); adapters wrap foreign errors with `cause`.
- **One central error handler** (HTTP + WS) maps errors to responses; non-`AppError` → `InternalError` (500, logged with stack, no internals exposed). Full catalogue: hub spec §8.
- **One error envelope** for every error (RFC 9457 Problem Details):
  `{"type":"…/errors/tank-offline","title":"Tank offline","status":503,"code":"tank_offline","detail":"…","requestId":"…"}`
  Error `code` values reuse the protocol's nack codes where applicable.
- Every external call (tank WS, ffmpeg, DB) has a **timeout**; reconnects use exponential backoff with jitter.
- **Never block the tank link:** fan-out is non-blocking; slow browser clients get the latest telemetry only (drop stale), never queue unboundedly.
- STOP paths bypass queues and are forwarded immediately.

### 4.4 Operational
- Structured JSON logs (pino) with `requestId` / `tankId` correlation; no secrets in logs; level policy and fields per hub spec §9.
- Prometheus metrics at `/metrics` per hub spec §10; `/healthz` + `/readyz`.
- Config from environment, validated at startup (fail fast on invalid config).
- Auth checked on every route and WS upgrade; tank keys stored encrypted at rest, never sent to browsers.

---

## 5. AI engine — Python (`ai-engine`)

### 5.1 Language and tooling
- Python ≥ 3.12, **type hints everywhere**, `pyright --strict` (or `mypy --strict`), `ruff` (lint + format).
- Layers: `domain/` (world model, skill logic — pure), `ports/` (`typing.Protocol`: `LlmProvider`, `Detector`, `DepthEstimator`, `CameraSource`, `TankClient`, `MemoryStore`), `adapters/` (Claude SDK, Ollama/MLX/vLLM, YOLO/SAM/Depth Anything, hub client, SQLite), `app/` (planner, scheduler), `main.py` (composition root).
- **Pydantic models** for every boundary (generated from `protocol/`); configuration via `pydantic-settings`, validated at startup.

### 5.2 Concurrency and real time
- `asyncio` with **structured concurrency** (`TaskGroup`); no fire-and-forget tasks.
- **Never block the event loop:** model inference runs in worker threads/processes; frames are dropped, not queued, when perception is behind (latest-frame-wins).
- **Every LLM/model/network call has a time budget** (`asyncio.timeout`) matching its router profile; on expiry → fallback chain → safe hold.

### 5.3 Error handling
- Exception hierarchy rooted at `ConquerorError` (`ProviderTimeout`, `ProviderUnavailable`, `PerceptionError`, `TankRejected`, `ConfigError`, …). No bare `except:`; no `except Exception` without re-raise or explicit handling.
- **Skills fail safe:** any unhandled error in a skill → skill stops → `stop` sent to the tank → error logged to the decision log.
- Retries only for idempotent calls, bounded, with jitter. Tank motion commands are **never** blindly retried.
- LLM output is **untrusted input:** tool calls are schema-validated before execution; invalid → rejected and reported back to the model, never executed.

### 5.4 LLM-specific rules
- Prompts are versioned files (not inline strings); tool schemas are generated from skill definitions (single source).
- Compact world-state summaries in prompts; images only when needed.
- Provider adapters isolate SDK specifics (Anthropic SDK for Claude; local runtimes behind the same port).
- Every LLM decision is logged (inputs summary, tool calls, latency, model) for the hub decision log.

### 5.5 Operational
- Structured logs (`structlog`, JSON), correlation by `tankId` / `commandId`.
- Secrets (API keys) from environment only; never logged or sent to the hub UI.
- Model downloads pinned by version/hash in the model registry.

---

## 6. Review checklist (every change)
- [ ] Layering respected (domain pure; dependencies inward; wiring only in the composition root)
- [ ] SOLID: single responsibility, extension without core edits, small ports
- [ ] Units explicit; no magic numbers
- [ ] Fail-safe behaviour on errors; no swallowed errors
- [ ] Firmware: no heap, no blocking, ISR rules, no `virtual` (Uno)
- [ ] Hub/AI: boundary validation, timeouts, typed errors, error envelope, no event-loop blocking
- [ ] Protocol changes made in `protocol/` and regenerated
- [ ] Lint/format/type-check clean; flash/RAM report checked (Uno)
