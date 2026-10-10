# ESP32 Firmware — Low-Level Design

Status: IN PROGRESS · Date: 2026-10-10 · Implements: Spec 2 Part B, Spec 1 · Standards: `docs/standards/coding-standards.md` §3

Sections: 1 Structure, tasks & frameworks ✅ · 2 Ports ✅ · 3 Body link & clock sync ✅ · 4 Client API · 5 Navigation · 6 Camera service · 7 Calibration engine · 8 WiFi, mDNS, OTA & storage · 9 Memory/task budget & error handling

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
