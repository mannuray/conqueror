# AI for the tank — what others build and what runs on our laptop (research, 2026-10-20)

Background for reopening the AI engine design (spec 04, design-notes §79). Two web-research passes: (1) hobby / open-source robots with LLMs or VLMs, (2) open-source tools and models on our laptop (Apple **M1, 16 GB**). Most speeds published online are for faster Macs or NVIDIA; "M1 guess" marks an estimate. **Benchmark on our laptop before deciding.** Links were checked by the research agents; anything they could not verify is marked.

## 1. What others have built

### Hobby robots driven by an LLM / VLM over WiFi
- **ELEGOO Smart Robot Car V4 + Claude via MCP** — ESP32 camera on home WiFi, a Python MCP server lets Claude send moves; the closest to us; a demo, no code found. https://pe.linkedin.com/in/ginorubio
- **SunFounder PiCar-X** — official GPT-4o-mini examples: a vision one ("what do you see") and a voice one (Vosk → GPT-4o-mini → Piper, wake word "Hey buddy"). https://docs.sunfounder.com/projects/picar-x-v20/en/latest/openai.html
- **FOFOCA (Espressif blog, 2026)** — tracked robot: ESP32 for motors and sensors only, Pi 5 for YOLOv8n / faces / Whisper / Piper, a local LLM server; a design reference. https://developer.espressif.com/blog/2026/05/fofoca-esp32-ai-robot/
- **XiaoZhi ESP32** (MIT, popular) — voice chatbot firmware; servos, LEDs, GPIO exposed as MCP tools that a cloud LLM calls. https://landscape.jimmysong.io/projects/xiaozhi-esp32/
- **Moondream "Bot Pursuit"** — type what to follow; the VLM finds it once, a fast visual-servo loop follows. VLM on a Pi: 29–56 s per query. https://forum.dexterindustries.com/t/moondream-vision-language-assistant-on-gopigo3-robot-kilted-dave/10750
- Teaching examples with a `take_pic` / `describe_surroundings` tool: look, act, look again. https://pypi.org/project/inspect-robots-agent/0.22.0/

**What works:** the LLM as a **planner that calls tools**; fast loops (obstacle stop, motors, tracking) on the microcontroller or a classic detector; the VLM only for "look, then act" or to pick a target once. No hobby project keeps an LLM in the per-frame loop successfully.

### Open-source stacks for small robots
- **LeRobot / LeKiwi** (Hugging Face) — learned policies from tele-operation recordings, mostly arms. https://wiki.seeedstudio.com/lerobot_lekiwi/
- **FrodoBots Earth Rover** + LeRobot; 2000 h driving dataset. https://huggingface.co/datasets/frodobots/FrodoBots-2K
- **ROSA (NASA JPL)** — a ReAct agent over ROS tools; Claude / OpenAI / Ollama. https://arxiv.org/html/2410.06472v2
- **ROS MCP servers** — Claude drives ROS robots through rosbridge; no built-in safety checks. https://glama.ai/mcp/servers/robotmcp/ros-mcp-server
- **OpenMind OM1**, **DimOS** — robot "agent OS" frameworks, mainly for Unitree-class robots. https://github.com/OpenMind/OM1 , https://pypi.org/project/dimos/
- **Anki Vector revival (wire-pod)** + **VectorIntelligence** — local gemma + Whisper, editable `persona.txt`, memory per face, moods, spontaneous greetings; "weak 2.4 GHz WiFi is the main cause of lag". https://github.com/Gman0909/VectorIntelligence
- **Waveshare UGV Rover** — Pi/Jetson host + ESP32 for motion (our split); no LLM demo found.
- **Google Gemini Robotics-ER** — API model that points at objects in a frame (normalised coordinates). https://ai.google.dev/gemini-api/docs/robotics-overview

### Toy robots — what people like
Loona (games, follow-me, expressive eyes), EMO (eye animations, dances, reactions to touch), Eilik (touch reactions only, still loved), Miko (stories, songs; mishears), Vector (personality, remembers faces). **Fun = fast reactions, small games, being remembered; deep conversation matters less; mishearing is the top complaint.**

### Children and safety
Little published: "adult present, not for under-3s, keep away from stairs" (Loona). Research: LLM-planned robots are easy to jailbreak (RoboPAIR, up to 100 % on a Jackal UGV, https://www.arxiv.org/abs/2410.13691v2); checking every plan against rules before it runs cut unsafe plans from > 92 % to < 3 % (RoboGuard, https://arxiv.org/abs/2503.07885). **Hard limits must sit outside the LLM** — our Uno / bridge envelope already does that. No hobby project publishes a child-play envelope like ours.

### A children's language + an LLM
ChatScratch (2024) and MindScratch (2024): the LLM proposes Scratch programs the child sees (https://arxiv.org/pdf/2402.04975 , https://arxiv.org/pdf/2412.09001). Sphero Edu's AI only **explains** the child's program, never writes it (https://help.sphero.com/sphero-support/using-the-sphero-edu-ai-assistant). No Logo / turtle + LLM robot found — **"the AI writes Tank Logo" looks new.**

## 2. What runs on our M1 16 GB

Key facts: the base M1 has ~68 GB/s memory bandwidth (M1 Max ~400), so local LLM/VLM text speed is low; the **Neural Engine is the same on M1 and M1 Max**, so CoreML numbers carry over.

| Job | Best fit | Speed | Licence |
|---|---|---|---|
| Detect + track (every frame) | **YOLO26n / YOLO11n via CoreML** + ByteTrack / BoT-SORT | measured on base M1: 7–8 ms at 256 px (ONNX + CoreML); guess 30–60 FPS at QVGA/VGA | AGPL-3.0 (Apache alternative: DEIMv2 / D-FINE) |
| Find by name ("the red cup") | YOLOE (text classes) or Florence-2 at ~1 Hz | guess ~1 s+ per frame for zero-shot models | AGPL / MIT |
| Depth from one camera | **Depth Anything V2 Small, Apple's CoreML build** | 32.8 ms on M1 Max Neural Engine → ~30 Hz on our M1; relative → scale with the ultrasonic | Apache 2.0 |
| Text-prompted masks | SAM 3 / 3.1 | official needs CUDA; MLX port untested; SAM 2.1 tiny ~1.6 s on M1 | SAM licence — **defer** |
| Local "what do you see" | **Qwen3-VL-4B / Qwen3.5-4B** via mlx-vlm; Moondream 2 `point()` for "where is it" | measured on base M1 16 GB: ~21 tok/s, 4 GB; **~4–5 s per frame** in total (calculated) | Apache 2.0 |
| Local tool-calling LLM | Qwen3.5-4B (same model as above) | ~21 tok/s; good at one tool call (88 %), weak at multi-step (22 %) | Apache 2.0 |
| Cloud brain | **Claude**: Haiku 5.5 fast, Sonnet / Opus 5.5 planning | a QVGA frame ≈ 108 tokens, VGA ≈ 414; cache the fixed prompt + tools; one image change breaks the message cache | — |
| Ready-made driving models (VLA) | none fits (SmolVLA / OpenVLA / pi0 are arm models; GNM / NoMaD weak at collisions) | — | — |
| Mapping | defer; later RTAB-Map (Homebrew, BSD, takes wheel odometry) | — | BSD-3 |
| Speech in | parakeet-mlx or Apple SpeechAnalyzer (macOS 26); push-to-talk first | ~0.5 s for a short clip (M4) | — |
| Speech out | **Kokoro-82M** (CoreML) | measured on an M1 Mini: 234 ms for a 3 s clip | Apache 2.0 |
| Tools for the LLM | Claude API tools or a small **MCP** server wrapping our WebSocket commands | MCP Python SDK just went 2.x (pin) | — |

**Our moves already fit the research:** NaVILA / Mobility VLA show that a slow planner (1–5 s per step) works when it gives **mid-level commands** ("move 75 cm", "turn 30°") that a lower layer carries out safely — exactly our bridge's move / turn / arc / pivot, which end on their own under the Uno's rules.

**Memory budget (estimate):** macOS + apps 4–5 GB, 4B VLM ~4 GB, YOLO + depth < 0.5 GB, speech 1–2 GB, Kokoro 0.3 GB → ~10–12 GB; an 8B model on top gets tight.

## 3. Lessons for us
1. **LLM as a planner calling a small set of tools** that map onto our bridge commands (move, turn, arc, pivot, look, snapshot, sound, LED, say) — never in the fast loop.
2. **Fast loops stay local and classic:** YOLO + tracker + depth on the Neural Engine at 10–20 Hz; steering through the bridge's drive-rate / move service.
3. **Look, act, look again** with small bounded moves; our ±1 cm moves make each step reliable.
4. **Safety stays below the LLM** (Uno + bridge envelope); add a rule check of every plan in the engine as a second layer; never expose Override mode to the AI.
5. **Tank Logo as the AI's output** for commands: a program the person can see, check, change and run — the checker is a guardrail too; "explain this program" is a cheap extra.
6. **Fun comes from fast reactions** (turn toward a face, sounds, LED, greetings, moods, small games), with an editable persona and local memory.
7. **WiFi quality** is the top practical problem; keep the link monitoring and degrade gracefully.
8. **Avoid big frameworks** (ROS 2, OM1, DimOS, LeRobot) for now — a thin Python engine fits an Uno + ESP32 robot.
9. **Plan for mishearing:** confirm voice commands before moving; a physical STOP always within reach.

## 4. Open risks
- Almost no base-M1 benchmarks — measure first.
- Picture quality of WiFi MJPEG (blur, JPEG artefacts) while driving is unknown for detection / depth.
- Frame-to-telemetry timing over WiFi is unknown (matters for memory and maps).
- Licences: Ultralytics AGPL, SAM / Moondream 3 / openWakeWord models have their own terms.
- Fast-moving runtimes (Ollama MLX preview, MCP SDK 2.x).
