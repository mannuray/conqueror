# Conqueror Platform — AI Engine (Design Spec 4)

Status: REVIEWED 2026-10-10 · Date: 2026-10-10 · Depends on: Spec 0, Spec 1, Spec 3

The AI engine is the project's **main offering**: a Python service, client of the hub, with **vision as the primary input**.
It is fully configurable so anyone can run it on what they have.

## 1. Requirements (user)
- All capability groups: natural-language commands, vision perception, autonomy/mapping, watch & alert, personality & fun.
- **First priority: natural-language commands.**
- Headline fun mode: **"don't let the baby catch you"** chase game.
- Depth perception and other current AI capabilities.
- LLM switchable **local ⇄ cloud**, and **fast** (no multi-minute thinking).
- Runs on whatever hardware people have.

## 2. Architecture

```
Hub ─video+telemetry─► [1 Frame ingest] ─► [2 Perception] ─► [3 World model]
                                                                 │
User command (text/voice) ─► [5 Agent / LLM planner] ◄─ queries ─┤
                                  │ selects & supervises         │
                                  ▼                              │
                           [4 Skills: 5–15 Hz loops] ─ reads ────┘
                                  │
                                  ▼  tank commands via hub (`ai` role; tank safety always applies)
```

### 2.1 Frame ingest
- Camera-source abstraction: hub MJPEG re-stream (default), H.264/RTSP, phone, USB (for future camera upgrades).
- Each frame paired with telemetry interpolated at its `X-Timestamp` (pose, heading, pan/tilt) → **pose-tagged frames**.
- Full-res snapshots (`/api/capture?res=full`, OV3660 3 MP) on demand for detail.

### 2.2 Perception pipeline (each stage: `local` / `cloud` / `off`, model, fps, resolution)
| Stage | Candidates (verify versions at implementation) | Output |
|---|---|---|
| Detect + track | YOLO family (fast); **SAM 3** (text-prompted concepts, segmentation + tracking) | tracked objects with image boxes/masks |
| Depth | **Depth Anything V2 Small / V3**, YOLO26-depth | relative depth → **metric** via ultrasonic anchor + camera intrinsics |
| Pose / gestures | body-pose models | posture, approach, point/wave gestures |
| 3D mapping | MASt3R-SLAM / VGGT-SLAM (fused with encoder pose) | 3D/2D map |
| Scene understanding | Claude vision or local VLM | descriptions, answers |
- Budget scheduler: cheap stages every frame, heavy stages on keyframes/on demand; drops frames rather than queueing (latency first).
- **Camera calibration** (checkerboard) → intrinsics + distortion (needed for metric depth; fisheye if a wide lens is fitted later).
- **Active vision:** skills and the planner can steer pan/tilt (scan, keep target centred, glance toward turns).

### 2.3 World model
- Tank state + pose (from ESP32), camera pose.
- Tracked objects with **world positions** (image bearing + pan/tilt + heading + depth) and last-seen time; **persistent semantic memory** (SQLite) — "where did you last see my keys?".
- 2D occupancy grid from depth + ultrasonic + pose.
- People tracks (position, velocity, approach direction).

### 2.4 Skills (fast closed loops; start/stop/status; declare hardware/tier requirements)
`look_around` · `track(target)` · `follow(target, distance)` · `keep_away(target)` · `find(object)` · `go_to(x,y)` (delegates to ESP32 nav) ·
`explore` · `patrol(route)` · `photograph(subject)` · `avoid_with_depth` · `guard` (watch + alert).

### 2.5 Agent / planner
- LLM with tools = skills + queries (`what_do_you_see`, `where_is`, `snapshot`, `say`, `beep`, `led`, `set_camera`).
- Runs on **user commands and events**, never in the control loop.
- Instant acknowledgement ("thinking" sound/LED) on every command.
- Prompt carries a compact **world-state summary**; images only when it needs to look.

## 3. LLM router
| Profile | Job | Target latency |
|---|---|---|
| Fast | everyday commands, reactions, chat | < 1–2 s |
| Smart | multi-step missions | < 5 s |
| Vision | scene questions | < 3 s |
- Each profile: provider (cloud/local), model, time budget, fallback chain. Budget exceeded → fallback (cloud ⇄ local) or safe hold + "not sure, try again". **Never waits indefinitely.**
- **Cloud (Anthropic SDK):** Claude Haiku 5.5 `claude-haiku-5-5` (fastest), Claude Sonnet 5.5 `claude-sonnet-5-5` (balanced), Claude Opus 5.5 `claude-opus-5-5` (best planning; thinking cannot be disabled → low effort; optional fast mode). Latency levers: low `effort`, streaming with eager tool-input streaming, prompt caching of system prompt + tools.
- **Local:** small tool-calling models via Ollama / llama.cpp / MLX / vLLM; chosen by benchmark at implementation time.
- **Offline mode:** local only when no internet.

## 4. Hardware tiers (auto-detected and benchmarked at first start; every setting overridable)
| Tier | Hardware | Runs locally |
|---|---|---|
| 0 Cloud | any laptop | tiny detector; VLM/LLM in cloud |
| 1 CPU/Pi | Pi 5, old PC | + YOLO-nano tracking, low-res depth 2–5 fps |
| 2 Apple Silicon | M1–M4 | + real-time depth, SAM 3 on demand, local SLM (MLX) |
| 3 NVIDIA | RTX-class | everything: real-time SAM 3, 3D mapping, larger local LLM/VLM |
- Features declare requirements; unavailable features are greyed out in the hub UI with the reason.
- Config: one YAML file + hub AI settings page; presets `cloud`, `pi`, `mac`, `nvidia`.
- Model registry downloads and caches the chosen models.

## 5. Modes and safety
- **Child-play mode ("don't let the baby catch you"):**
  - Requires the `child_play` envelope (speed ≤ 20 %, gentle accel, brake locked) — **enforced by the Uno**; the AI cannot loosen it.
  - Requires the hub start confirmation (adult present, track guards, flat area).
  - Child-play skills run **only while slow telemetry reports the envelope active**; if it disappears they stop immediately.
  - `keep_away(baby)`: retreat/circle, never approach — **AI-level rule only** (the boards cannot see); camera depth helps but is not trusted for stairs/drop-offs.
  - Any bump/lift → the Uno stops; the skill pauses and re-evaluates.
- Other fun modes: follow-me, hide-and-seek, pet play, photographer, guard/patrol with alerts, personality engine.
- **Personality engine:** simple mood state (curious, happy, scared, tired on low battery) → sound/LED/motion style.
- **Voice:** STT/TTS through the hub (browser/phone mic and speaker), local or cloud.

## 6. Observability and tools
- Decision log (inputs seen, plan, skill transitions, why) streamed to the hub AI page.
- Per-stage latency and fps.
- **Replay mode:** re-run recorded sessions (video + telemetry from the hub) through perception/skills offline.
- Camera benchmark integration (Spec 3 §3).

## 7. Out of scope (now)
VLA robot-action models (SmolVLA, NORA, OpenVLA, π0) — arm-oriented; revisit later.

## 8. Frozen stack (DECIDED 2026-10-10)

### 8.1 Layout
```
ai-engine/
├── src/conqueror_ai/
│   ├── domain/      world model, skill logic, mode envelopes (pure)
│   ├── ports/       typing.Protocol: LlmProvider, Vlm, Detector, Segmenter, DepthEstimator, PoseEstimator,
│   │                GestureRecognizer, Mapper, Embedder, CameraSource, HubClient, MemoryStore, Stt, Tts
│   ├── adapters/    concrete providers (below)
│   ├── app/         planner, skill runner, perception scheduler, LLM router
│   ├── api/         internal FastAPI (localhost): /health/live, /health/ready, /metrics, debug views (dev)
│   ├── generated/   protocol pydantic models + error registry (from /protocol)
│   └── main.py      composition root
├── config/          presets: cloud.yaml, pi.yaml, mac.yaml, nvidia.yaml
└── prompts/         versioned prompt files
```

### 8.2 Core
| Concern | Choice |
|---|---|
| Python | 3.12 (pinned) |
| Packaging | uv + pyproject + lockfile; extras `[cpu]`, `[cuda]`, `[mac]` |
| Concurrency | asyncio TaskGroups; inference in a thread pool (GIL released by PyTorch/ONNX); latest-frame-wins |
| Models / config | pydantic v2 (generated from `protocol/`), pydantic-settings + YAML presets |
| Hub link | `websockets` (WS) + `httpx` (async HTTP, MJPEG stream) |
| Internal HTTP | **FastAPI + uvicorn on localhost** (health, metrics, dev debug views; same error envelope as the hub) |
| Image I/O | PyTurboJPEG, OpenCV, numpy |
| Storage | SQLite (aiosqlite) for world memory + decision log; sqlite-vec for embeddings |
| Errors | `ConquerorError` (code + reason from the shared registry, `AI_7xxx`; `is_operational`; safe details) → `ProviderTimeout`, `ProviderUnavailable`, `PerceptionError`, `TankRejected`, `ToolCallInvalid`, `ConfigError` |
| Logging | structlog JSON; contextvars carry requestId/commandId + tankId (forwarded from the hub); secret redaction |
| Metrics / tracing | OpenTelemetry SDK (OTLP optional) + Prometheus exporter: per-stage latency/fps/drops, LLM latency/timeouts/fallbacks per provider/model/profile, tool-call rejections, skill runs/fail-safes, GPU/memory, hub link |
| Tooling | ruff, pyright strict, pytest available |
| Deployment | Docker `ai-engine:cpu` (multi-arch) and `ai-engine:cuda`; **macOS runs natively** (`uv run`) because Docker on Mac cannot use the GPU (Metal/MPS) |

### 8.3 Capability defaults (one default per capability; others are optional adapters)
| Capability | Default | Runtime / notes |
|---|---|---|
| Object detection | **Ultralytics YOLO** (n/s by tier) | ONNX on CPU, PyTorch on Mac/NVIDIA; **AGPL accepted** |
| Find by name (open vocabulary) | **YOLOE** (Ultralytics) real-time; **SAM 3** on demand for precise masks | SAM 3 on Tier 2–3 or via on-demand calls; SAM License |
| Tracking + re-ID | Ultralytics **BoT-SORT** | |
| Body pose | Ultralytics **YOLO-pose** | |
| Hand gestures | **MediaPipe** Gesture Recognizer | CPU |
| **Depth** | **Depth Anything V2 Small**, all tiers (Pi/CPU 256 px ~2–5 fps; Mac/NVIDIA 518 px real-time); relative depth scaled to cm by the **ultrasonic anchor** | Apache; ONNX/HF |
| 2D map | own occupancy-grid code (depth + ultrasonic + encoder pose) | numpy |
| Visual memory | **SigLIP** embeddings + sqlite-vec | |
| Calibration / motion detection | **OpenCV** | |
| LLM planner | cloud **Claude Haiku/Sonnet/Opus 5.5** (Anthropic SDK); local via **Ollama** / **mlx-lm** / **vLLM** | per router profile |
| Scene understanding (VLM) | **Claude vision**; local **Moondream** | |
| Speech-to-text | **faster-whisper** + Silero VAD | cloud optional |
| Text-to-speech | **Kokoro** | cloud optional |
| **Optional plugins (off by default)** | 3D SLAM (MASt3R-SLAM — non-commercial licence / VGGT), **face identity** (local enrolment only), alternative depth models (YOLO26-depth, DA V3, Video Depth Anything, Depth Pro) | enabled per config; swapped via ports if the camera/depth benchmark shows a gain |

- **Model registry:** source, exact version/hash, licence, size, minimum tier for every model; shown on the hub AI page.
- **Child vs adult** is inferred from body size/pose (no faces needed); face identity stays off unless explicitly enabled.
- The camera benchmark tool includes a **depth benchmark** (fps, latency, error vs ultrasonic/tape) for evaluating alternative depth adapters later.
