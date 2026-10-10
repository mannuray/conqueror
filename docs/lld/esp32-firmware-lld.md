# ESP32 Firmware — Low-Level Design

Status: IN PROGRESS · Date: 2026-10-10 · Implements: Spec 2 Part B, Spec 1 · Standards: `docs/standards/coding-standards.md` §3

Sections: 1 Structure, tasks & frameworks ✅ · 2 Ports ✅ · 3 Body link & clock sync ✅ · 4 Client API ✅ · 5 Navigation ✅ · 6 Camera service ✅ · 7 Calibration engine · 8 WiFi, mDNS, OTA & storage · 9 Memory/task budget & error handling

Note: UART is **250000 baud** and calibration maths runs here (Uno LLD §9 revision).

---

## 1. Structure, tasks & frameworks (APPROVED)

### 1.1 Framework choices
| Concern | Choice | Why |
|---|---|---|
| Base | Arduino-ESP32 core on PlatformIO | reuses ELEGOO camera code |
| HTTP + WS + MJPEG | ESP-IDF `esp_http_server`, two instances (80: API/WS/setup, 81: stream) | already used by the camera example; native WS; one server stack |
| JSON | ArduinoJson 7 with a fixed memory-pool allocator (no heap in hot paths) | standard, fast, follows allocation rule |
| Hashing | mbedTLS: salted SHA-256 for tokens, PBKDF2 for the admin password | built in |
| Storage | NVS (Preferences) | wear-levelled |
| Protocol | generated C++ from `protocol/` (frames + JSON mapping) | single source |

### 1.2 Layers
```
src/
├── main.cpp       composition root: services, queues, tasks
├── app/           FreeRTOS tasks: read ports/queues → call domain → write ports/queues
├── domain/        pure C++: arbitration, auth rules, navigation, derived metrics, JSON⇄command mapping
├── ports/         headers (camera, uart, wifi, nvs, http, mdns, ota, clock)
├── drivers/       ESP32 implementations (IDF / Arduino)
└── generated/     frames, JSON schemas, error registry
```
`virtual` only where ≥ 2 implementations are active at runtime.

### 1.3 Tasks, cores, queues
```mermaid
classDiagram
    direction LR
    class BodyLinkTask { <<core 1 · prio 6>> +run() UART RX/TX, heartbeat 100 ms, HELLO/version check }
    class ControlTask { <<core 1 · prio 5>> +run() WS commands → arbitration → frames }
    class NavigationTask { <<core 1 · prio 4>> +run() 20 Hz pose + mission step }
    class TelemetryTask { <<core 1 · prio 3>> +run() state → JSON tf/ts/ev broadcast }
    class CameraTask { <<core 0 · prio 3>> +run() MJPEG to ≤2 viewers, capture }
    class HttpServer80 { <<core 0 · IDF server task>> REST, /ws, captive portal }
    class WifiTask { <<core 0 · prio 1>> +run() connect / fallback AP / mDNS }
    class OtaTask { <<core 0 · prio 1>> +run() firmware upload → flash }
    class CommandQueue { <<queue 16>> ClientCommand }
    class FrameTxQueue { <<queue 16>> Frame }
    class TankState { <<single writer + mutex snapshot>> Uno telemetry, owner, flags, pose }
    class EventBus { <<queue 32>> TankEvent }
    HttpServer80 --> CommandQueue : WS messages (parsed, authenticated)
    ControlTask --> CommandQueue : consumes
    ControlTask --> FrameTxQueue : produces
    NavigationTask --> FrameTxQueue : primitives
    BodyLinkTask --> FrameTxQueue : consumes
    BodyLinkTask --> TankState : writes
    BodyLinkTask --> EventBus : Uno events
    NavigationTask --> TankState : pose
    TelemetryTask --> TankState : snapshot
    TelemetryTask --> EventBus : consumes → broadcast
    CameraTask --> TankState : frame timestamps
```
- Core 1 = robot-critical (link, control, navigation, telemetry); core 0 = WiFi stack, HTTP, camera, OTA → video load cannot delay STOP.
- Tasks communicate only via queues + `TankState` (single writer per field group, mutex snapshots for readers).
- **STOP fast path:** WS STOP is pushed to the front of `CommandQueue` (`xQueueSendToFront`) and forwarded immediately.
- Task watchdog on all app tasks; reboot reason kept in RTC memory and reported at next boot.

### 1.4 Startup sequence
```mermaid
sequenceDiagram
    participant M as main
    participant N as NVS
    participant W as WifiTask
    participant B as BodyLinkTask
    participant C as CameraTask
    participant H as HttpServer
    M->>N: load config (WiFi, keys, envelope, camera, missions)
    M->>C: init camera (sensor PID → per-sensor settings)
    M->>B: start UART, heartbeats
    B-->>M: Uno HELLO (fw, proto) → compatible?
    M->>B: restore envelope from NVS (brake stays ON)
    M->>W: saved networks (≤15 s) else AP + captive portal
    W-->>M: IP + mDNS conqueror-XXXX
    M->>H: start port 80 (API/WS) + port 81 (stream)
```

---

## 2. Ports (APPROVED)
Headers of free functions implemented in `drivers/`; no `virtual` needed.

```mermaid
classDiagram
    direction LR
    class clock { <<port>> +nowUs() int64 +nowMs() uint32 }
    class body_uart { <<port>> +begin(baud=250000, rx=IO33, tx=IO4) +read(uint8*, size) size_t +write(const uint8*, size) size_t +txFree() size_t }
    class camera { <<port>> +init(CameraConfig) SensorInfo +grab() FrameRef +release(FrameRef) +apply(CameraSettings) Status +captureFull() FrameRef }
    class wifi { <<port>> +connectSta(const Creds*, n, timeout_ms) WifiResult +startAp(ssid, pass) +scan() ScanList +status() WifiStatus +rssi() int8 }
    class captive_dns { <<port>> +start() +stop() }
    class mdns { <<port>> +start(hostname, ServiceTxt) +update(ServiceTxt) }
    class http { <<port>> +start(port, max_sockets) Server +on(Server, uri, method, Handler) +wsSendAsync(Server, fd, data, size, text) Status +wsClose(Server, fd) +sessionCount(Server) uint8 }
    class nvs { <<port>> +get(ns, key, void*, size) Status +put(ns, key, const void*, size) Status +erase(ns, key) Status }
    class ota { <<port>> +begin(size) Status +write(data, size) Status +end() Status +abort() +markValid() +rollbackPending() bool }
    class crypto { <<port>> +sha256(...) +pbkdf2(pass, salt, iters, out) +randomBytes(uint8*, size) }
    class sys { <<port>> +restart() +resetReason() ResetReason +freeHeap() uint32 +freePsram() uint32 +rtcPut(RebootInfo) +rtcGet(RebootInfo&) bool }
    class FrameRef { <<struct>> +const uint8* data +size_t len +uint16 w,h +int64 ts_us +void* handle }
    class SensorInfo { <<struct>> +uint16 pid +SensorKind kind }
    class CameraSettings { <<struct>> framesize · quality · vflip · hmirror · brightness · contrast · saturation }
    class Creds { <<struct>> +char ssid[33] +char pass[64] }
    camera ..> FrameRef
    camera ..> SensorInfo
    camera ..> CameraSettings
    wifi ..> Creds
```
`SensorKind {OV2640, OV3660, OV5640}`.

| Port | Notes | Thread/core rules | On failure |
|---|---|---|---|
| clock | esp_timer µs since boot (64-bit) | any task | — |
| body_uart | UART2 @ 250000, 1 KB RX / 512 B TX | **only BodyLinkTask** | short read/write counted |
| camera | esp_camera, PSRAM, 2 frame buffers; `ts_us` = capture time in clock time | **only CameraTask** (others via queue) | grab timeout 200 ms → CAMERA_TIMEOUT; reinit after 5 consecutive |
| wifi | STA ≤ 3 creds, AP fallback, scan | **only WifiTask** | WifiResult (no network / wrong password / timeout) |
| captive_dns | all queries → 192.168.4.1 in AP setup | WifiTask | — |
| mdns | `conqueror-XXXX`, `_conqueror._tcp`, TXT id/name/proto/fw | WifiTask | retried on IP change |
| http | esp_http_server; :80 max 7 sockets (6 WS + 1 REST); :81 2 stream sockets | handlers in server task; WS sends async | send failure → session closed |
| nvs | namespaces wifi, keys, tank, missions, cal, envelope | any task, short ops | Status |
| ota | dual partitions + rollback (must reach Uno link OK + WiFi up within 60 s) | OtaTask | abort keeps old image |
| crypto | mbedTLS; PBKDF2 10 000 rounds | any task | — |
| sys | RTC memory keeps reboot reason + WDT culprit task | any task | — |

Rules: each hardware resource has **one owning task** (others via queues; no locks around hardware) · `domain/` never includes `ports/`; shared value types in `types/` · pins/sizes in `drivers/board_config.h` (camera pin map `CAMERA_MODEL_M5STACK_WIDE`, UART IO33/IO4).

---

## 3. Body link, clock sync & derived metrics (APPROVED)

### 3.1 Class diagram
```mermaid
classDiagram
    direction TB
    class BodyLinkTask {
      -FrameParser parser_
      -FrameWriter writer_
      -UnoSession session_
      -AckTracker acks_
      -ClockSync clock_
      -TelemetryDecoder decoder_
      +run()
    }
    class UnoSession {
      -SessionState state_
      -uint32 last_rx_ms_
      -UnoHello hello_
      +onHello(UnoHello, now) SessionAction
      +onFrame(now)
      +update(now) SessionAction
      +state() SessionState
    }
    class AckTracker {
      -Pending pending_[16]
      +track(seq, client_fd, cmd_id, now)
      +onAck(seq, status) AckResult
      +expire(now) AckResults
    }
    class ClockSync {
      -int64 offset_ms_
      -RttWindow rtt_
      +onTelem(uno_ms, esp_rx_ms)
      +onAckRtt(ms)
      +toTankMs(uno_ms) uint32
      +linkLatencyMs() uint16
    }
    class TelemetryDecoder {
      +decodeFast(Frame, TankFast&) bool
      +decodeSlow(Frame, TankSlow&) bool
      +decodeEvent(Frame, TankEvent&) bool
    }
    class TankState {
      <<single writer per group, mutex snapshot>>
      +TankFast fast
      +TankSlow slow
      +Derived derived
      +Pose pose
      +Owner net_owner
      +snapshot() TankSnapshot
    }
    class DerivedMetrics {
      -BatteryModel battery_
      -TtcCalc ttc_
      -SlipCalc slip_
      -RoughnessScale rough_
      -LifetimeStats life_
      +update(TankFast, TankSlow, dt) Derived
    }
    class BatteryModel { +pct(mv) uint8 +sag() float +runtimeMin() uint16 }
    class TtcCalc { +ttc(dist_mm, closing_cm_s) float }
    class SlipCalc { +update(encL, encR, yaw_rate, track_width, dt) uint8 }
    class RoughnessScale { +scale(accel_var, speed) uint8 }
    class LifetimeStats { +add(distance, dt) +persistDue(now) bool }
    class EventTranslator { +toClient(TankEvent) ClientEvent }
    BodyLinkTask *-- UnoSession
    BodyLinkTask *-- AckTracker
    BodyLinkTask *-- ClockSync
    BodyLinkTask *-- TelemetryDecoder
    BodyLinkTask --> TankState
    BodyLinkTask --> DerivedMetrics
    BodyLinkTask --> EventTranslator
    DerivedMetrics *-- BatteryModel
    DerivedMetrics *-- TtcCalc
    DerivedMetrics *-- SlipCalc
    DerivedMetrics *-- RoughnessScale
    DerivedMetrics *-- LifetimeStats
```
Writers: fast/slow/derived = BodyLinkTask · pose = NavigationTask · net_owner = ControlTask. `FrameParser`/`FrameWriter` are the same generated C++ as the Uno.

### 3.2 Uno session state machine
```mermaid
stateDiagram-v2
    [*] --> Disconnected
    Disconnected --> Syncing : HELLO (proto major matches)
    Disconnected --> Incompatible : HELLO (proto differs) → body_incompatible
    Syncing --> Synced : ENVELOPE (from NVS) + HELLO_ACK sent
    Synced --> Lost : no valid frame 300 ms → body_offline
    Lost --> Syncing : HELLO or frames resume → HELLO_REQ
    Synced --> Syncing : new HELLO while synced (Uno reset) → re-sync
    Incompatible --> Syncing : HELLO with matching proto
```
- Not Synced → client motion commands get `BODY_OFFLINE` / `BODY_INCOMPATIBLE`; camera keeps streaming.
- Heartbeat every 100 ms in every state.

### 3.3 Commands and acks
- ControlTask → `FrameTxQueue` (with client ref) → BodyLinkTask sends + tracks ack (seq → client, cmd id, 200 ms deadline).
- Uno ACK status → registry code/reason → client `ack`/`nack`; no ack in 200 ms → `nack COMMAND_TIMEOUT`.
- **Motion commands never auto-resent**; settings/calibration may retry once.
- Streaming (DRIVE, CAM_JOY) untracked; Uno's rate-limited rejection events → `nack` to the owning client.

### 3.4 Clock sync
- `tank_ms` = ESP32 time; offset = `esp_rx_ms − uno_ms` as the **minimum over a sliding window**; Uno timestamps converted.
- Link latency = EMA ack RTT / 2 → `esp.link_latency_ms`.
- Camera frames stamped in ESP32 time (same clock as telemetry); hub estimates its offset via ping/pong.

### 3.5 Derived metrics
| Metric | Method |
|---|---|
| batt_pct | 2S Li-ion LUT (8.4 V → 100 %, 6.6 V → 0 %) on idle voltage |
| sag_v | idle EMA − loaded EMA (`driving` flag) |
| runtime_min | voltage slope while driving → minutes to critical |
| ttc_s | dist / closing speed when closing > 2 cm/s, else null |
| slip_pct | `(vR − vL)/track_width` vs gyro rate during turns |
| roughness | accel_var normalised by speed → 0–100 |
| lifetime | encoder distance + runtime + power-ons; NVS every 5 min |

### 3.6 Telemetry path
```mermaid
sequenceDiagram
    participant U as Uno
    participant B as BodyLinkTask
    participant S as TankState
    participant T as TelemetryTask
    participant N as NavigationTask
    participant C as Clients (WS)
    U->>B: TELEM_FAST (20 Hz)
    B->>B: decode + clock map + derived metrics
    B->>S: write fast + derived
    N->>S: snapshot → integrate pose → write pose
    T->>S: snapshot at each subscriber rate (≤ 20 Hz)
    T->>C: tf JSON (latest-value, drop stale)
    U->>B: EVENT
    B->>T: EventBus (translated, tank_ms)
    T->>C: ev JSON
```

---

## 4. Client API — WebSocket, auth, arbitration, REST (APPROVED)

### 4.1 Class diagram
```mermaid
classDiagram
    direction TB
    class WsGateway {
      +onOpen(fd)
      +onText(fd, data, len)
      +onClose(fd)
    }
    class SessionRegistry {
      -ClientSession sessions_[6]
      +open(fd) ClientSession*
      +byFd(fd) ClientSession*
      +close(fd)
      +forEach(fn)
    }
    class ClientSession {
      +int fd
      +SessionState state
      +Role role
      +uint8 key_id
      +char client_name[24]
      +Subscriptions subs
      +RateLimiter limiter
      +uint32 last_rx_ms
      +OutSlots out
    }
    class JsonCodec {
      <<generated>>
      +parse(data, len, ClientMessage&) ParseResult
      +encode(...) size_t
    }
    class Authorizer {
      <<generated table>>
      +allowed(Role, MsgType) bool
    }
    class AuthService {
      +verifyKey(token) KeyRecord*
      +verifyAdminPassword(pass) bool
      +createKey(name, Role) Token
      +revoke(key_id)
      +firstRun() bool
      -LoginThrottle throttle_
    }
    class KeyStore { <<NVS>> +records() +put(KeyRecord) +erase(id) }
    class KeyRecord { <<struct>> +uint8 id +char name[24] +Role role +uint8 salt[16] +uint8 hash[32] +uint32 created +uint32 last_used }
    class CommandRouter { +route(ClientCommand) RouteResult }
    class NetworkArbiter {
      -Owner owner_
      -int owner_fd_
      -uint32 last_motion_ms_
      +onMotion(ClientSession&, now) Decision
      +onStop(ClientSession&) Decision
      +onRelease(ClientSession&) Decision
      +onIrOwner(bool ir_active)
      +update(now) Decision
    }
    class EnvelopeService {
      -Envelope current_
      +apply(Role, EnvelopeCmd) Result
      +setBrake(Role, bool on) Result
      +restoreFromNvs() Envelope
    }
    class RestRouter { +info() +settings() +keys() +wifi() +ota() +missions() +capture() }
    class LoginThrottle { +allow(ip, now) bool +fail(ip, now) }
    WsGateway --> SessionRegistry
    WsGateway ..> JsonCodec
    WsGateway --> AuthService
    WsGateway ..> Authorizer
    WsGateway --> CommandRouter : via CommandQueue (ControlTask)
    SessionRegistry *-- ClientSession
    AuthService --> KeyStore
    AuthService *-- LoginThrottle
    KeyStore ..> KeyRecord
    CommandRouter --> NetworkArbiter
    CommandRouter --> EnvelopeService
    RestRouter --> AuthService
```
`SessionState {Connected, Authenticated, Closing}` · `Role {viewer, ai, human, admin}` · `Owner {None, Ai, Human}`. Keys: salted SHA-256 + constant-time compare; admin password PBKDF2; login throttle 5 failures → 30 s lockout.

### 4.2 Session lifecycle
```mermaid
stateDiagram-v2
    [*] --> Connected : WS upgrade
    Connected --> Authenticated : hello valid key → welcome
    Connected --> Closing : bad key → AUTH_FAILED · no hello 5 s
    Authenticated --> Authenticated : messages (rate-limited)
    Authenticated --> Closing : silent 10 s · key revoked · 5 protocol errors
    Closing --> [*]
```
Owner session closing → STOP + owner released.

### 4.3 Network arbitration (human > AI)
```mermaid
stateDiagram-v2
    [*] --> None
    None --> Ai : AI motion
    None --> Human : human motion
    Ai --> Human : human motion → STOP, control_changed(human)
    Human --> Human : same session motion (timer reset)
    Human --> None : 5 s idle or release_control → STOP, control_changed(none)
    None --> Ai : AI motion → control_returned (if preempted)
    Ai --> None : AI idle 1 s / closed → STOP
```
- Other same-role sessions → `NOT_OWNER` until the timer expires.
- IR owner (from Uno telemetry) → all network motion `NOT_OWNER`; arbiter frozen.
- STOP from any authenticated session (incl. viewer) → queue front → Uno.
- Frames tagged `src`; `OWNER_HINT` sent to the Uno for LED colour.

### 4.4 Validation pipeline
1. Parse (generated codec, fixed pool) → `INVALID_COMMAND`
2. Authorize (role × type table) → `ROLE_FORBIDDEN`
3. Range-validate (generated from schema) → `INVALID_COMMAND` + field
4. Rate limit (drive/cam_joy ≤ 25 Hz, others ≤ 10/s per session) → `RATE_LIMITED`
5. Capability (`move`/`nav_*` without encoders → `UNAVAILABLE`; body not synced → `BODY_OFFLINE`)
6. Arbitrate (motion) → `NOT_OWNER` / accepted
7. Route: body frame → FrameTxQueue · nav → NavigationTask · camera config → CameraTask · settings/keys → local · ping → pong

### 4.5 Envelope rules
| Action | ai | human/admin |
|---|---|---|
| set / tighten | ✅ tighter only | ✅ |
| loosen / clear | ❌ `ENVELOPE_LOCKED` | ✅ |
| obstacle brake off | ✅ unless `lock_brake` | ✅ unless `lock_brake` |
Envelope persisted in NVS and restored to the Uno on every HELLO.

### 4.6 REST (port 80)
`/api/info`, `/api/settings` (GET human+, PUT admin, ETag), `/api/keys`, `/api/wifi`, `/api/ota`, `/api/missions`, `/api/capture` (→ CameraTask queue). Auth: `Authorization: Bearer` or `?key=`. Errors: shared code/reason registry in the hub's envelope `{success:false, error:{…}}`.

### 4.7 Human takes over from the AI
```mermaid
sequenceDiagram
    participant AI as AI client (ai key)
    participant H as Human app (human key)
    participant G as WsGateway
    participant A as NetworkArbiter
    participant B as BodyLinkTask
    AI->>G: drive v=30 (owner Ai)
    G->>A: onMotion(ai)
    A-->>B: DRIVE src=ai
    H->>G: drive v=50
    G->>A: onMotion(human)
    A-->>B: STOP (owner change)
    A-->>G: ev control_changed(human)
    A-->>B: DRIVE src=human
    AI->>G: drive
    G-->>AI: nack NOT_OWNER
    Note over H: 5 s without input
    A-->>B: STOP
    A-->>G: control_changed(none) · control_returned → AI
```

---

## 5. Navigation (APPROVED)

### 5.1 Class diagram
```mermaid
classDiagram
    direction TB
    class NavigationTask {
      -PoseEstimator pose_
      -MissionRunner runner_
      +run()
      +onCommand(NavCommand)
      +onPrimitiveDone(PrimitiveDone)
    }
    class PoseEstimator {
      -Pose pose_
      -int32 last_enc_[2]
      -bool valid_
      +update(TankFast, counts_per_cm) Pose
      +setOrigin()
      +homeDistanceCm() float
    }
    class Pose { <<struct>> +float x_cm +float y_cm +float theta_deg +bool valid }
    class MissionRunner {
      -RunState state_
      -Route route_
      -uint8 idx_
      -uint8 replans_
      -uint32 blocked_since_
      -SourceRef owner_
      +start(Route, speed, SourceRef) Result
      +cancel(Reason)
      +step(Pose, TankFast, now) RunnerAction
      +onPrimitiveDone(PrimitiveDone)
      +progress() Progress
    }
    class PathPlanner { +plan(Pose, Waypoint target, Waypoint* next) Segments }
    class Segment { <<struct>> +SegKind kind +float value +uint16 radius_cm +uint8 speed }
    class Route { <<struct>> +Waypoint pts[32] +uint8 n +bool loop +bool return_heading }
    class MissionStore { <<NVS>> +list() +get(id) Mission +put(Mission) +erase(id) }
    class Mission { <<struct>> +uint8 id +char name[24] +Route route +uint8 speed }
    NavigationTask *-- PoseEstimator
    NavigationTask *-- MissionRunner
    MissionRunner --> PathPlanner
    PathPlanner ..> Segment
    MissionRunner ..> Route
    MissionStore ..> Mission
    PoseEstimator ..> Pose
```
`SegKind {TurnTo, TurnArc, MoveDist, FinalHeading}`; MissionStore ≤ 8 missions × 32 points.

### 5.2 Pose integration (20 Hz)
- `Δd = mean(ΔencL, ΔencR) / counts_per_cm`; θ = Uno gyro yaw (source of truth; encoders not trusted for heading).
- Midpoint integration `x += Δd·cos(θ_mid)`, `y += Δd·sin(θ_mid)`.
- Uno reboot (counts jump to 0) → skip step, keep pose, raise `pose_degraded`.
- No encoders → `valid = false`, nav commands `UNAVAILABLE`.
- `nav_set_origin` → (0,0,0); `home_dist_cm` published.

### 5.3 Path planning
- Heading change ≤ 90° and distance ≥ 2 × radius → **arc** (`TURN to bearing`, radius `min(30 cm, dist/2)`) + `MOVE` remainder; else **spin** + `MOVE`.
- Residual > 5 cm after MOVE → re-plan (max 3), then continue with `waypoint_inaccurate`.
- Return home = go-to (0,0) then turn to θ = 0.

### 5.4 Mission runner state machine
```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Planning : start(route), owner = requester
    Planning --> Turning : TurnTo / TurnArc → TURN
    Planning --> Moving : MoveDist → MOVE
    Turning --> Planning : primitive_done ok
    Moving --> Arrived : primitive_done ok
    Arrived --> Planning : residual > 5 cm and replans < 3
    Arrived --> Planning : next waypoint
    Arrived --> Done : last waypoint (loop → first)
    Moving --> Blocked : obstacle brake engaged
    Blocked --> Moving : clear within 5 s → re-plan
    Blocked --> Failed : 5 s → BLOCKED
    Turning --> Failed : primitive_done fail
    Moving --> Failed : primitive_done fail
    Planning --> Failed : owner lost → PREEMPTED
    Turning --> Failed : link lost → LINK_LOST
    Moving --> Failed : link lost → LINK_LOST
    Done --> Idle : nav_done ok
    Failed --> Idle : STOP sent
```
- Missions run under the requester's ownership (frames tagged with its source, normal arbitration); preemption → `PREEMPTED`, never silently resumed.
- Progress via `primitive` telemetry and `nav_progress` events.

### 5.5 Go-to sequence
```mermaid
sequenceDiagram
    participant C as Client
    participant G as WsGateway/ControlTask
    participant N as NavigationTask
    participant B as BodyLinkTask
    participant U as Uno
    C->>G: nav_goto {x:120, y:40, speed:40}
    G->>N: NavCommand (owner = C)
    N->>N: plan → TurnArc(18°, r=30) + Move(118 cm)
    N->>B: TURN to 18° radius 30
    B->>U: frame
    U-->>B: primitive_done ok
    B-->>N: event
    N->>B: MOVE 118 cm at 18°
    U-->>B: primitive_done ok
    N->>N: residual 3 cm → arrived
    N-->>C: ev nav_done ok
```

---

## 6. Camera service (APPROVED)
Problem solved: ELEGOO's stream handler loops inside the server task (second viewer blocked). Design: capture once, share to ≤ 2 viewers via refcounted frames and per-viewer async tasks.

### 6.1 Class diagram
```mermaid
classDiagram
    direction TB
    class CameraTask {
      -SensorProfile profile_
      -FramePool pool_
      -FpsMeter fps_
      +run()
      +onRequest(CameraRequest)
    }
    class SensorProfile {
      <<per sensor PID>>
      +SensorKind kind
      +CameraSettings defaults
      +framesize_full
      +apply(sensor_t*)
    }
    class FramePool {
      -SharedFrame frames_[3]
      +acquire() SharedFrame*
      +publish(SharedFrame*)
      +latest() SharedFrame*
      +release(SharedFrame*)
    }
    class SharedFrame {
      +uint8* jpeg
      +size_t len
      +uint16 w, h
      +uint32 tank_ms
      +uint32 seq
      +atomic refcount
    }
    class StreamServer {
      <<port 81>>
      -ViewerSlot viewers_[2]
      +onStreamRequest(req)
    }
    class ViewerTask {
      <<one per viewer, core 0>>
      -int fd
      -uint32 last_seq_
      +run()
    }
    class CaptureService { +captureFull(reply_to) }
    class FpsMeter { +tick() +fps() float }
    CameraTask *-- SensorProfile
    CameraTask *-- FramePool
    CameraTask *-- FpsMeter
    FramePool *-- SharedFrame
    StreamServer *-- ViewerTask
    ViewerTask --> FramePool
    CaptureService --> CameraTask
```

### 6.2 Streaming
- CameraTask grabs → copies JPEG into a refcounted PSRAM `SharedFrame` (≈ 25–60 KB @ 640×480) → returns the driver buffer at once → publishes as latest.
- One `ViewerTask` per viewer (max 2) using `httpd_req_async_handler_begin`; waits for a newer frame, sends one part, releases. **Latest-frame-wins**: slow viewers skip frames; the camera is never slowed.
- 3rd viewer → `503` ("use the hub").
- Each part: `X-Timestamp: <tank_ms>` (capture time, ESP32 clock) + `X-Frame-Seq`.
```
--frame
Content-Type: image/jpeg
Content-Length: 41233
X-Timestamp: 1234567
X-Frame-Seq: 9876
<jpeg bytes>
```
```mermaid
sequenceDiagram
    participant D as esp_camera driver
    participant C as CameraTask
    participant P as FramePool
    participant V1 as ViewerTask (app)
    participant V2 as ViewerTask (hub)
    D-->>C: fb (DMA done, timestamp)
    C->>P: acquire → memcpy jpeg → stamp tank_ms, seq
    C->>D: esp_camera_fb_return(fb)
    C->>P: publish(frame)
    P-->>V1: notify
    P-->>V2: notify
    V1->>P: latest() → send part → release
    V2->>P: latest() → send part → release
```

### 6.3 Full-resolution capture
```mermaid
sequenceDiagram
    participant R as REST /api/capture?res=full
    participant C as CameraTask
    participant S as Sensor
    R->>C: CaptureRequest(full)
    C->>S: framesize QXGA (2048×1536)
    C->>C: discard 2 frames (settle)
    C->>C: grab → SharedFrame
    C->>S: restore stream framesize
    C-->>R: JPEG + X-Timestamp
```
Stream pauses ~0.5 s (viewers keep the last frame); one full capture at a time, queue 2, then `BUSY`.

### 6.4 Sensor profiles, settings, health
- Sensor PID at boot → profile: **OV3660** (stock), OV2640, OV5640 (defaults: flip/mirror/quality, full framesize).
- Default stream 640×480, JPEG quality 12; admin changes via CameraTask queue; persisted in NVS.
- Grab timeout 200 ms → counter; 5 consecutive → re-init + `camera_reset` event; `esp.fps` + dropped-frame counters in telemetry.

### 6.5 Memory
PSRAM: 2 driver buffers + 3 pool frames ≈ 300 KB (+ ~400 KB temporary for a 3 MP capture) of 4 MB; internal RAM untouched by video.
