# Uno Firmware — Low-Level Design

Status: IN PROGRESS · Date: 2026-10-10 · Implements: Spec 2 Part A, Spec 1 §8 · Standards: `docs/standards/coding-standards.md` §2

Sections: 1 Structure & execution ✅ · 2 Ports ✅ · 3 Motion control ✅ · 4 Safety & arbitration ✅ · 5 Comms ✅ · 6 Sensors & drivers ✅ · 7 Settings, calibration, self-test · 8 Memory budget & error handling

Every section includes classes and Mermaid diagrams (class, sequence, state).

---

## 1. Structure & execution model (APPROVED)

### 1.1 Directory layout (PlatformIO, `firmware/uno/`)
```
src/
├── main.cpp                    composition root: static objects, wiring, setup(), loop()
├── app/                        task classes: read ports → call domain → write ports
│   ├── scheduler.h/.cpp        fixed task table, due-time dispatch, load measurement
│   ├── control_task.*          100 Hz control chain (arbitration → … → safety → motors)
│   ├── imu_task.*              200 Hz IMU read + estimation
│   ├── sensor_tasks.*          ultrasonic 20 Hz, battery 10 Hz, button 50 Hz
│   ├── camera_task.*           servos 50 Hz
│   ├── ui_task.*               buzzer + LED 50 Hz
│   ├── comms_dispatcher.*      frame → command routing
│   └── telemetry_builder.*     TELEM_FAST / TELEM_SLOW / EVENT frames
├── domain/                     PURE C++: no Arduino.h, no port calls (inputs → outputs)
│   ├── motion/                 speed_controller, heading_controller, mixer, ramp, deadband,
│   │                           battery_comp, primitives, turn_profile
│   ├── safety/                 safety_supervisor, envelope
│   ├── arbitration/            arbitrator
│   ├── camera/                 servo_profile, camera_head
│   ├── ui/                     sound_sequencer, led_sequencer, catalogues (PROGMEM)
│   ├── estimation/             attitude_estimator, yaw_integrator, encoder_speed,
│   │                           bump_detector, lift_detector
│   ├── calibration/            calibration_runner + routines
│   ├── selftest/               self_test
│   ├── settings/               settings (typed struct + defaults + CRC)
│   └── util/                   pid.h, ring_buffer.h, units.h, crc8.h, filters.h
├── ports/                      headers only: namespaced free functions (link-time swap)
├── drivers/                    Uno implementations of ports (Arduino.h allowed only here)
└── generated/                  protocol frames + error registry (from /protocol)
test-fakes/                     host implementations of ports
```

### 1.2 Design rules applied
- **Functional core, imperative shell:** `domain/` classes take inputs and return outputs; they never touch hardware. `app/` task classes read ports, call domain, write ports.
- **Ports** are headers of namespaced free functions (`port::motor::write(...)`); the implementation is chosen at link time (`drivers/` on the Uno, `test-fakes/` on a PC). No `virtual`.
- **Safety by structure:** only `ControlTask` includes `ports/motor_output.h`, and it writes only the `SafetySupervisor` output.
- All objects static; no heap.

### 1.3 Top-level class diagram
```mermaid
classDiagram
    direction LR
    class Scheduler {
      -Task tasks_[N]
      +run()
      +loadPct() uint8_t
      +maxLoopUs() uint16_t
      +overruns() uint16_t
    }
    class Task {
      <<struct>>
      +void (*fn)()
      +uint32_t period_us
      +uint32_t next_due_us
    }
    class ControlTask { +tick() }
    class ImuTask { +tick() }
    class SensorTasks { +ultrasonicTick() +batteryTick() +buttonTick() }
    class CameraTask { +tick() }
    class UiTask { +tick() }
    class CommsDispatcher { +poll() +dispatch(Frame) }
    class TelemetryBuilder { +fastTick() +slowTick() +event(EventKind,int16,int16) }

    class Arbitrator
    class Primitives
    class HeadingController
    class SpeedController
    class SafetySupervisor
    class Envelope
    class AttitudeEstimator
    class YawIntegrator
    class EncoderSpeed
    class BumpDetector
    class LiftDetector
    class CameraHead
    class SoundSequencer
    class LedSequencer
    class CalibrationRunner
    class SelfTest
    class Settings

    Scheduler o-- Task
    Scheduler ..> ControlTask : calls 100 Hz
    Scheduler ..> ImuTask : 200 Hz
    Scheduler ..> SensorTasks
    Scheduler ..> CameraTask : 50 Hz
    Scheduler ..> UiTask : 50 Hz
    Scheduler ..> CommsDispatcher : every loop
    Scheduler ..> TelemetryBuilder : 20 Hz / 1 Hz

    ControlTask --> Arbitrator
    ControlTask --> Primitives
    ControlTask --> HeadingController
    ControlTask --> SpeedController
    ControlTask --> SafetySupervisor
    ControlTask --> CalibrationRunner
    SafetySupervisor --> Envelope
    ImuTask --> AttitudeEstimator
    ImuTask --> YawIntegrator
    ImuTask --> BumpDetector
    ImuTask --> LiftDetector
    ControlTask --> EncoderSpeed
    CameraTask --> CameraHead
    UiTask --> SoundSequencer
    UiTask --> LedSequencer
    CommsDispatcher --> Arbitrator : network commands
    CommsDispatcher --> Settings
    CommsDispatcher --> CalibrationRunner
    CommsDispatcher --> CameraHead
    CommsDispatcher --> UiTask
    TelemetryBuilder ..> Settings
```
(Members of the domain classes are detailed in their own sections.)

### 1.4 Layer / port dependency diagram
```mermaid
classDiagram
    direction TB
    class ports {
      <<headers>>
      clock · motor_output · encoders · imu · ultrasonic · servos
      tone · led · ir · battery_adc · button · link · eeprom · watchdog
    }
    class drivers {
      <<Uno, Arduino.h>>
      implement ports
    }
    class test_fakes {
      <<host PC>>
      implement ports
    }
    class app { <<tasks>> }
    class domain { <<pure C++>> }
    app ..> ports : reads / writes
    app ..> domain : calls
    drivers ..|> ports
    test_fakes ..|> ports
```

### 1.5 Execution model
- `loop()` → `scheduler.run()`: fixed task table (period + next-due), run-to-completion, never blocking; time from the clock port (`micros()`), wrap-safe.
- Periods: comms every loop · IMU 200 Hz · control 100 Hz · servos/UI/button 50 Hz · ultrasonic 20 Hz · telemetry 20 Hz fast / 1 Hz slow · battery 10 Hz.
- **Timing budget (16 MHz):** IMU ~0.4 ms @200 Hz, control chain ~1.5 ms @100 Hz, telemetry ~0.3 ms → expected load 30–40 %; measured `loop_load`, `loop_max_us`, overrun count reported in TELEM_SLOW.
- **Overrun rule:** a late task's next due = now + period (no catch-up bursts).
- **Watchdog:** 1 s, kicked by the scheduler after each full pass; reset reason reported at next boot.

### 1.6 Control tick sequence (100 Hz)
```mermaid
sequenceDiagram
    participant S as Scheduler
    participant C as ControlTask
    participant A as Arbitrator
    participant P as Primitives
    participant H as HeadingController
    participant V as SpeedController
    participant F as SafetySupervisor
    participant M as port::motor
    S->>C: tick()
    C->>C: read port::encoders, imu snapshot, clock
    C->>A: update(now, ir/network inputs)
    A-->>C: active MotionCommand (source, v, ω or primitive)
    C->>P: update(command, yaw, distance travelled)
    P-->>C: MotionTarget (v_pct, ω_dps, mode)
    C->>H: update(target, yaw, yaw_rate, dt)
    H-->>C: track Δ
    C->>V: update(v_pct ± Δ, measured track speeds, battery V, dt)
    V-->>C: PwmRequest (left, right)
    C->>F: gate(PwmRequest, SafetyInputs)
    F-->>C: PwmOutput + flags/events
    C->>M: write(left, right)
```

---

## 2. Ports (APPROVED)
Each port = header of namespaced free functions (`port::<name>::fn`), no `virtual`; Uno build links `drivers/*.cpp`, host build links `test-fakes/*.cpp`.

```mermaid
classDiagram
    direction LR
    class clock { <<port>> +nowUs() uint32_t +nowMs() uint32_t }
    class motor { <<port>> +init() +write(int16 left, int16 right) +brake() }
    class encoders { <<port>> +init(bool enabled) +snapshot() EncoderSnapshot }
    class imu { <<port>> +init() Status +read(ImuRaw&) Status +whoAmI() uint8_t }
    class ultrasonic { <<port>> +trigger() +poll(uint16_t& echo_us) EchoState }
    class servos { <<port>> +attach(Axis) +writeUs(Axis, uint16 us) +detach(Axis) }
    class tone { <<port>> +play(uint16 hz) +stop() }
    class led { <<port>> +show(Rgb) }
    class ir { <<port>> +poll(IrEvent&) bool }
    class battery_adc { <<port>> +readMv() uint16_t }
    class button { <<port>> +pressed() bool }
    class link { <<port>> +readByte(uint8_t&) bool +write(const uint8_t*, uint8 len) bool +txFree() uint8_t }
    class eeprom { <<port>> +read(uint16 addr, void*, uint16 len) +update(uint16 addr, const void*, uint16 len) }
    class watchdog { <<port>> +enable() +kick() +resetReason() ResetReason }

    class EncoderSnapshot { <<struct>> +int32 count_l +int32 count_r +uint32 period_l_us +uint32 period_r_us +uint32 t_us }
    class ImuRaw { <<struct>> +int16 ax,ay,az +int16 gx,gy,gz +int16 temp +uint32 t_us }
    class IrEvent { <<struct>> +uint32 code +bool repeat +uint32 t_us }
    class Rgb { <<struct>> +uint8 r,g,b }
    class Status { <<enum>> Ok · NotFound · BusError · Saturated }
    class EchoState { <<enum>> Pending · Ready · Timeout }
    class Axis { <<enum>> Pan · Tilt }
    class ResetReason { <<enum>> PowerOn · External · BrownOut · Watchdog }

    encoders ..> EncoderSnapshot
    imu ..> ImuRaw
    imu ..> Status
    ir ..> IrEvent
    led ..> Rgb
    ultrasonic ..> EchoState
    servos ..> Axis
    watchdog ..> ResetReason
```

| Port | Units / range | Timing | ISR safety | On failure |
|---|---|---|---|---|
| clock | µs / ms, wrap 2³² | O(1) | anywhere | — |
| motor | −255..255 (sign = direction, 0 = coast); `brake()` | < 20 µs | **only ControlTask includes it** | — |
| encoders | counts (sign from commanded direction), last pulse period µs | atomic snapshot (IRQs briefly off) | counting in ISRs (D3 INT1, A1 PCINT1) | disabled → zeros |
| imu | raw MPU6050 counts (±250 °/s, ±2 g) | ~0.4 ms I2C with timeout | main loop | BusError/NotFound, last values kept |
| ultrasonic | echo µs (cm = µs/58) | `trigger()` 10 µs pulse; echo timed by ISR; `poll()` non-blocking | echo edge in PCINT0 ISR | Timeout after 25 ms (no obstacle in range) |
| servos | 500–2500 µs | instant (Servo lib, Timer1) | main loop | — |
| tone | 31–20000 Hz | non-blocking (Timer2) | main loop | — |
| led | 0–255 per channel | ~30 µs IRQs off; call only on change | main loop | — |
| ir | NEC 32-bit code + repeat | decoded in PCINT0 ISR; 4-entry queue | queue ISR-safe | malformed dropped + counted |
| battery_adc | mV (divider scale applied) | ~120 µs ADC | main loop | — |
| button | debounced 20 ms | O(1) | main loop | — |
| link | bytes; 64 B RX ring, 64 B TX | non-blocking; `write` false if no room | serial ISRs in core | write false → frame dropped + counted |
| eeprom | bytes, 1 KB | `update` writes changed bytes only | main loop, never during motion bursts | — |
| watchdog | 1 s | `kick()` once per scheduler pass | — | reason from MCUSR |

Rules:
- ISRs only in `drivers/`, capture-only. The shared PCINT0 ISR (D9 IR, D12 echo) reads PINB once, diffs, forwards edges to the IR decoder and echo timer.
- `domain/` never includes `ports/`; only `app/` does.
- All pins/hardware constants in one file: `drivers/board_config.h` (matches Spec 2 §A1).

---

## 3. Motion control (APPROVED)

### 3.1 Pipeline
```
MotionCommand ─► Primitives ─► MotionTarget ─► HeadingController ─► Δ (turn effort)
                                    │ v                                    │
                                    └──────────────► Mixer (L = v−Δ, R = v+Δ, pivot fallback)
                                                          │ track targets %
                                     Ramp (accel limit) ◄─┘
                                          │
                     SpeedController [OpenLoop | EncoderPid]  ◄── EncoderSpeed
                          │ uses Deadband + BatteryComp
                          ▼
                     PwmRequest (L, R) ──► SafetySupervisor (§4)
```

### 3.2 Class diagram
```mermaid
classDiagram
    direction TB
    class MotionCommand {
      <<struct>>
      +Source source
      +CmdKind kind
      +int8 v_pct
      +int8 w_pct
      +int16 yaw_rate_ddps
      +TurnMode turn_mode
      +int16 angle_ddeg
      +uint16 radius_cm
      +bool pivot
      +int32 dist_mm
      +int16 heading_ddeg
      +uint8 speed_pct
      +uint32 t_ms
    }
    class MotionTarget {
      <<struct>>
      +float v_pct
      +TargetMode mode
      +float yaw_rate_dps
      +float heading_deg
      +bool pivot
    }
    class Primitives {
      -State state_
      -TurnProfile turn_
      -MoveProfile move_
      +accept(MotionCommand) Result
      +update(Inputs) MotionTarget
      +state() State
      +progressPct() uint8
      +takeDone(PrimitiveDone&) bool
    }
    class TurnProfile {
      +start(start_deg, target_deg, speed_pct, radius_cm)
      +update(yaw_deg, yaw_rate_dps, dt) TurnStep
    }
    class MoveProfile {
      +start(dist_mm, speed_pct, heading_deg)
      +update(travelled_mm, dt) MoveStep
    }
    class HeadingController {
      -Pid~float~ heading_pid_
      -Pid~float~ rate_pid_
      -float held_heading_
      -bool holding_
      -uint16 saturated_ms_
      +update(MotionTarget, yaw_deg, yaw_rate_dps, v_pct, dt) float
      +turnLimited() bool
      +reset()
    }
    class TurnLimits {
      <<value>>
      +float rate_max_dps
      +float rate_at_full_speed_pct
      +ReverseSteer reverse_steer
      +maxRate(v_pct) float
    }
    class Mixer {
      +mix(v_pct, delta_pct, pivot_fallback, keep_pct) TrackTargets
    }
    class TrackTargets { <<struct>> +float left_pct +float right_pct }
    class Ramp {
      -float out_[2]
      +apply(TrackTargets, accel_pct_s, dt) TrackTargets
      +reset()
    }
    class SpeedController {
      -Mode mode_
      -Pid~float~ pid_[2]
      -Deadband deadband_
      -BatteryComp battery_
      +setMode(Mode)
      +update(TrackTargets, TrackSpeeds, float batt_v, dt) PwmRequest
      +reset()
    }
    class Deadband {
      -uint8 start_[4]
      -uint8 keep_[4]
      -uint16 kick_ms_[2]
      +map(track, float pct, bool from_standstill, dt) int16
    }
    class BatteryComp {
      +float v_nominal
      +float motor_v_max
      +scale(int16 pwm, float batt_v) int16
    }
    class EncoderSpeed {
      -int32 last_count_[2]
      -Method method_[2]
      +update(EncoderSnapshot, int8 cmd_sign_l, int8 cmd_sign_r, dt) TrackSpeeds
      +travelledMm() float
      +faulty(track) bool
    }
    class TrackSpeeds { <<struct>> +float cps[2] +float pct[2] +bool valid }
    class PwmRequest { <<struct>> +int16 left +int16 right }
    class Pid~T~ {
      +T kp, ki, kd
      +T out_min, out_max, i_max
      +update(T setpoint, T measured, T dt) T
      +reset()
      +saturated() bool
    }
    Primitives *-- TurnProfile
    Primitives *-- MoveProfile
    Primitives ..> MotionCommand
    Primitives ..> MotionTarget
    HeadingController *-- Pid
    HeadingController ..> TurnLimits
    SpeedController *-- Pid
    SpeedController *-- Deadband
    SpeedController *-- BatteryComp
    SpeedController ..> TrackSpeeds
    SpeedController ..> PwmRequest
    Mixer ..> TrackTargets
    Ramp ..> TrackTargets
    EncoderSpeed ..> TrackSpeeds
```
Enums: `CmdKind {Drive, DriveRate, Turn, Move, Stop, Cancel}` · `TurnMode {By, To}` · `TargetMode {HoldHeading, Rate, Spin}` · `SpeedController::Mode {OpenLoop, EncoderPid}` (VARIANT POINT) · `EncoderSpeed::Method {Window, Period}` · `ReverseSteer {Car, Tank}`.

### 3.3 Primitives state machine
```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Teleop : Drive / DriveRate
    Teleop --> Teleop : Drive stream (≤20 Hz)
    Teleop --> Idle : Stop / v=0,w=0 settled
    Idle --> Turning : Turn
    Teleop --> Turning : Turn (preempts)
    Idle --> Moving : Move (needs encoders)
    Teleop --> Moving : Move (preempts)
    Turning --> Idle : |err|<1° and |rate|<5°/s for 100 ms → done ok
    Turning --> Idle : timeout (2× expected + 2 s) → done fail
    Moving --> Idle : remaining < 5 mm → done ok
    Moving --> Idle : stall / blocked 5 s → done fail
    Turning --> Teleop : Drive (preempts, done cancelled)
    Moving --> Teleop : Drive (preempts, done cancelled)
    Turning --> Idle : Cancel / Stop
    Moving --> Idle : Cancel / Stop
```

### 3.4 Algorithms
- **HeadingController (cascaded):** `w = 0, v ≠ 0` → heading hold (capture *current* heading when `w` returns to 0; heading PID → rate setpoint). `w ≠ 0` → rate mode, ω = `w% × maxRate(v)`; car-style reverse inverts ω when `v < 0`. Inner loop: turn-rate PID + feedforward `Δ = ω × track_width_eff / 2` (→ %). Anti-windup (integral + output clamp); `turnLimited` after 300 ms saturated.
- **TurnProfile:** ω = `min(max_rate, √(2·decel·|error|))`; radius → `v = ω·r` (arc), none → `v = 0` (spin).
- **MoveProfile:** trapezoid (accel, cruise = speed_pct, decel) with heading hold; distance from `EncoderSpeed::travelledMm()`.
- **Mixer:** `L = v − Δ`, `R = v + Δ`; if > 100 % scale both while preserving the difference (turning priority); pivot fallback: inner target below keep threshold (non-zero) → 0.
- **SpeedController:** OpenLoop `pwm = deadband.map(target%)`; EncoderPid `target_cps = target% × max_cps[track]`, `pwm = ff(target) + PID(target_cps, measured)`; encoder fault → OpenLoop + `encoder_fault`. Then BatteryComp: `pwm × V_nom / V_batt`, capped to `motor_v_max` (6 V).
- **Deadband:** `(0,100] → [keep..255]` per track/direction; 60 ms kick to `start` from standstill.
- **EncoderSpeed:** window method (counts / 20 ms) when ≥ 8 counts per window, else period method; direction from command; fault = driven above deadband with no counts 0.5 s.
- Controllers use `float` internally; integers on the wire.

---

## 4. Safety & arbitration (APPROVED)

### 4.1 Class diagram
```mermaid
classDiagram
    direction TB
    class Arbitrator {
      -Owner owner_
      -uint32 ir_last_ms_
      -uint32 drive_last_ms_
      -uint16 ir_handback_ms_
      +onNetwork(MotionCommand) Result
      +onIr(IrEvent, now) IrAction
      +update(now, link_alive) ArbitrationOut
      +owner() Owner
      +takeEvent(ArbEvent&) bool
    }
    class IrKeymap {
      <<PROGMEM table>>
      +lookup(uint32 code) IrKey
    }
    class IrAction {
      <<struct>>
      +ActionKind kind
      +MotionCommand motion
      +CameraCommand camera
      +UiCommand ui
    }
    class ArbitrationOut {
      <<struct>>
      +MotionCommand active
      +Owner owner
      +uint32 drive_age_ms
      +bool stop_requested
    }
    class SafetySupervisor {
      -FaultSet latched_
      -SafetyState state_
      -uint16 level_ok_ms_
      -uint16 bump_hold_ms_
      -float stop_ramp_
      +gate(PwmRequest, SafetyInputs, dt) SafetyOut
      +state() SafetyState
      +flags() uint16
      +takeEvent(SafetyEvent&) bool
      +clearStall()
    }
    class SafetyInputs {
      <<struct>>
      +uint16 dist_cm
      +bool dist_valid
      +float pitch_deg
      +float roll_deg
      +bool lifted
      +bool bump
      +float bump_g
      +bool stall_l
      +bool stall_r
      +float batt_v
      +uint32 drive_age_ms
      +bool link_alive
      +Owner owner
      +bool selftest_ok
    }
    class SafetyOut {
      <<struct>>
      +PwmOutput pwm
      +StopMode stop_mode
      +bool blocked
      +bool limited
    }
    class FaultSet {
      <<bitmask>>
      Tilt · Lifted · Bump · StallL · StallR
      BattLow · BattCritical · Deadman · LinkLost · SelfTestLock
    }
    class Envelope {
      +uint8 speed_cap_pct
      +uint8 accel_cap_pct_s
      +bool lock_brake
      +apply(EnvelopeCmd) Result
      +active() bool
    }
    class BrakeConfig {
      +bool obstacle_brake_on
      +uint8 stop_cm
      +uint8 slow_cm
      +set(bool on, const Envelope&) Result
    }
    class SafetyThresholds {
      <<from Settings>>
      +uint8 tilt_limit_deg
      +float bump_g
      +uint16 deadman_ms
      +float batt_low_v
      +float batt_crit_v
    }
    Arbitrator ..> IrKeymap
    Arbitrator ..> IrAction
    Arbitrator ..> ArbitrationOut
    SafetySupervisor *-- FaultSet
    SafetySupervisor --> Envelope
    SafetySupervisor --> BrakeConfig
    SafetySupervisor ..> SafetyThresholds
    SafetySupervisor ..> SafetyInputs
    SafetySupervisor ..> SafetyOut
```
Enums: `Owner {None, Network, Ir}` · `ActionKind {Motion, Camera, Ui, Speed, Calibrate, None}` · `SafetyState {Normal, Limited, SoftStop, Halted, Locked}` · `StopMode {None, Smooth, Brake}`. `BrakeConfig.obstacle_brake_on` is always true at boot.

### 4.2 Owner state machine
```mermaid
stateDiagram-v2
    [*] --> None
    None --> Network : network motion command
    Network --> None : idle beyond handback window
    None --> Ir : IR motion key
    Network --> Ir : IR motion key → STOP, ir_override, control_changed
    Ir --> None : 3 s without IR keys or IR OK → STOP, control_changed
    Ir --> Ir : network motion → rejected NOT_OWNER
```
- The ESP32 already arbitrated human vs AI; the Uno sees "network" tagged with its source.
- Non-motion IR keys act via `IrAction` without changing the owner. STOP from any source always applies.

### 4.3 Safety supervisor state machine
```mermaid
stateDiagram-v2
    [*] --> Locked : self-test critical fail
    [*] --> Normal : self-test ok
    Normal --> Limited : obstacle slow zone / envelope active
    Limited --> Normal : clear
    Normal --> SoftStop : deadman / link lost
    Limited --> SoftStop : deadman / link lost
    SoftStop --> Normal : fresh command after recovery
    Normal --> Halted : tilt / lifted / bump / critical battery
    Limited --> Halted : tilt / lifted / bump / critical battery
    SoftStop --> Halted : hard fault
    Halted --> Normal : all latched faults cleared
```

### 4.4 `gate()` rules (every 10 ms, in order)
1. **Locked** → motors 0.
2. **Hard faults → Halted, brake now:** tilt > limit (clears after level 1 s) · lifted (clears on landed) · bump > threshold (300 ms hold) · critical battery 2 s (clears above crit + 0.3 V hysteresis).
3. **Stall (per track):** cut that track until the commanded direction changes or 1 s.
4. **Deadman / link lost → SoftStop:** ramp to 0 over ~300 ms; recover on a fresh command.
5. **Obstacle brake (if on):** split into `v = (L+R)/2`, `Δ = (R−L)/2`; if `v > 0` scale `v` by `clamp((dist − stop)/(slow − stop), 0, 1)`; recombine. Reverse and spin stay allowed; `blocked` at 0.
6. **Envelope:** final `|v| ≤ speed_cap`; accel cap also passed to the Ramp.
7. Output + events (blocked/unblocked, tilted/level, lifted/landed, bump{g}, stall{side}, deadman_stop, batt_low/critical, envelope_changed).

Ownership: the Uno applies envelope/brake; the **ESP32 enforces who may change them**; the Uno still rejects "brake off" while `lock_brake` (defence in depth).

### 4.5 IR override sequence
```mermaid
sequenceDiagram
    participant IR as IR remote
    participant A as Arbitrator
    participant F as SafetySupervisor
    participant T as TelemetryBuilder
    participant E as ESP32
    IR->>A: ▲ key (NEC frame)
    A->>A: owner Network → Ir
    A-->>F: stop_requested (one tick)
    A-->>T: events ir_override, control_changed(ir)
    T->>E: EVENT frames + owner in TELEM_SLOW
    E-->>E: network commands now get NOT_OWNER
    IR->>A: repeat codes (held)
    A-->>F: Drive v=speed%, w=0
    Note over A: 3 s without keys or OK key
    A->>A: owner Ir → None, STOP
    A-->>T: control_changed(none)
```

---

## 5. Comms — ESP32 link, Uno side (APPROVED)
Frames per Spec 1 §8: `0xAA | len | type | seq | payload | CRC-8`, 115200 baud.

### 5.1 Class diagram
```mermaid
classDiagram
    direction TB
    class FrameParser {
      -ParseState state_
      -uint8 buf_[48]
      -uint8 len_, idx_, crc_
      +feed(uint8 byte) bool
      +frame() const Frame&
      +errors() ParseErrors
    }
    class Frame {
      <<struct>>
      +uint8 type
      +uint8 seq
      +uint8 len
      +uint8 payload[48]
    }
    class ParseErrors {
      <<struct>>
      +uint16 bad_crc
      +uint16 bad_len
      +uint16 unknown_type
      +uint16 tx_dropped
    }
    class FrameWriter {
      -uint8 seq_
      +send(uint8 type, const void* payload, uint8 len) bool
    }
    class Codec {
      <<generated>>
      +decode(const Frame&, Msg&) bool
      +encode(const Msg&, uint8* out) uint8
    }
    class CommsDispatcher {
      -Route routes_[N]
      +poll()
      -dispatch(const Frame&)
      -ack(uint8 seq, AckStatus)
    }
    class Route {
      <<struct, PROGMEM>>
      +uint8 type
      +handler(const Frame&)
      +bool needs_ack
    }
    class LinkMonitor {
      -uint32 last_valid_ms_
      +onValidFrame(now)
      +alive(now) bool
    }
    class EventQueue {
      -Event ring_[8]
      +push(EventKind, int16 a, int16 b) bool
      +pop(Event&) bool
      +dropped() uint16
    }
    class TelemetryBuilder {
      +fastTick()
      +slowTick()
      +flushEvents()
      +sendHello()
    }
    class TxScheduler {
      +pump()
    }
    CommsDispatcher *-- FrameParser
    CommsDispatcher o-- Route
    CommsDispatcher --> LinkMonitor
    CommsDispatcher ..> Codec
    CommsDispatcher --> FrameWriter : acks
    TelemetryBuilder ..> Codec
    TelemetryBuilder --> EventQueue
    TxScheduler --> FrameWriter
    TxScheduler --> TelemetryBuilder
    FrameParser ..> Frame
    FrameParser ..> ParseErrors
```
`ParseState {Sync, Len, Type, Seq, Payload, Crc}`. `Codec` has one decode/encode pair per message type (generated).

### 5.2 Parser state machine
```mermaid
stateDiagram-v2
    [*] --> Sync
    Sync --> Len : byte == 0xAA
    Sync --> Sync : other byte (skip)
    Len --> Type : len ≤ 48
    Len --> Sync : len > 48 → bad_len++
    Type --> Seq
    Seq --> Payload : len > 0
    Seq --> Crc : len == 0
    Payload --> Payload : idx < len
    Payload --> Crc : idx == len
    Crc --> Sync : crc ok → frame_ready
    Crc --> Sync : crc bad → bad_crc++
```

### 5.3 Routing table (OCP through data, PROGMEM)
| Type | Handler → target | Ack |
|---|---|---|
| HEARTBEAT | LinkMonitor | no |
| DRIVE, DRIVE_RATE | `Arbitrator::onNetwork` (streaming) | no (rejections → EVENT) |
| STOP, TURN, MOVE, CANCEL | `Arbitrator::onNetwork` | yes |
| CAM_JOY | CameraHead (streaming) | no |
| CAM_SET, CAM_NUDGE, CAM_HOME, CAM_LOCK | CameraHead | yes |
| LED, SOUND, TONE, AUDIO_MODE, OWNER_HINT | UiTask | yes (OWNER_HINT no) |
| BRAKE, ENVELOPE | BrakeConfig / Envelope | yes (`ENVELOPE_LOCKED` if refused) |
| SETTING_SET / GET / SAVE | Settings | yes / SETTING_VALUE |
| CALIBRATE, PROFILE | CalibrationRunner / Settings | yes, then CAL_RESULT progress |
| unknown | — | unknown_type++ |
Ack status = generated `COMMAND_3xxx` subset as a one-byte registry index.

### 5.4 Sending
- `TxScheduler::pump()` every loop, priority **ACK > EVENT > HELLO > TELEM_FAST > TELEM_SLOW**, only if `txFree()` fits the whole frame.
- No room → telemetry skipped (`tx_dropped`); events stay queued (ring 8, overflow counted).
- TELEM_FAST ≈ 46 B × 20 Hz ≈ 0.9 KB/s of 11.5 KB/s.
- HELLO at boot, repeated every 1 s until the first heartbeat, and on request.

### 5.5 Link health
- `alive` = valid frame within 300 ms → feeds the safety supervisor (link lost → SoftStop).
- `uno_ms` in every TELEM_FAST for ESP32 clock mapping.

### 5.6 Drive command flow
```mermaid
sequenceDiagram
    participant E as ESP32
    participant L as port::link (UART)
    participant D as CommsDispatcher
    participant P as FrameParser
    participant A as Arbitrator
    participant C as ControlTask
    E->>L: 0xAA 03 10 seq [src v w] crc
    D->>L: readByte() × n
    D->>P: feed(byte)
    P-->>D: frame_ready
    D->>D: route[0x10] → onDrive
    D->>A: onNetwork(Drive v,w)
    A-->>D: Accepted (or NOT_OWNER → EVENT)
    Note over C: next 10 ms tick uses the new command
    C->>C: arbitration → primitives → … → safety → motors
```

---

## 6. Sensors, estimation & drivers (APPROVED)

### 6.1 Drivers (`drivers/`, implement ports)
```mermaid
classDiagram
    direction LR
    class Drv8835Motor { <<driver: motor>> PWM D5/D6 Timer0 · DIR D8/D7 }
    class EncoderIsr { <<driver: encoders>> D3 INT1 rising · A1 PCINT1 · volatile counts + edge µs }
    class Mpu6050 { <<driver: imu>> I2C 0x68 400 kHz · burst 0x3B ×14 · Wire timeout 3 ms }
    class PcInt0Hub { <<ISR>> PINB diff → NecDecoder.edge / EchoTimer.edge }
    class EchoTimer { <<driver: ultrasonic>> TRIG D13 · ECHO D12 · rise/fall µs }
    class NecDecoder { <<driver: ir>> edge state machine · 4-entry queue }
    class Ws2812 { <<driver: led>> D4 bit-bang 800 kHz · 24 bits }
    class ServoDriver { <<driver: servos>> Servo lib Timer1 · D10 pan · D11 tilt }
    class ToneDriver { <<driver: tone>> tone() Timer2 · A0 }
    class BatteryAdc { <<driver: battery_adc>> A3 · 4-sample avg · divider scale }
    class ButtonDriver { <<driver: button>> D2 pull-up · 20 ms debounce }
    class UartLink { <<driver: link>> Serial 115200 · 64/64 B }
    class EepromStore { <<driver: eeprom>> EEPROM.update }
    class WatchdogDriver { <<driver: watchdog>> wdt 1 s · MCUSR captured early }
    class BoardConfig { <<header>> all pins + constants }
    PcInt0Hub --> NecDecoder
    PcInt0Hub --> EchoTimer
    Drv8835Motor ..> BoardConfig
    EncoderIsr ..> BoardConfig
    Mpu6050 ..> BoardConfig
```
MPU6050 init: `PWR_MGMT_1=0x01` (PLL), `SMPLRT_DIV=4` (200 Hz), `CONFIG=0x03` (~44 Hz DLPF), gyro ±250 °/s, accel ±2 g; `WHO_AM_I=0x68` checked by self-test.

| ISR | Pins | Work (capture only, < 10 µs) |
|---|---|---|
| INT1_vect | D3 left encoder | `count_l += dir_l`, edge time |
| PCINT1_vect | A1 right encoder | same, right |
| PCINT0_vect | D9 IR, D12 echo | read PINB, diff, forward edge + µs |
| USART, TIMER0/1/2 | — | Arduino core, Servo, tone |

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> LeadMark : falling edge
    LeadMark --> LeadSpace : mark ≈ 9 ms
    LeadMark --> Idle : other
    LeadSpace --> Bits : space ≈ 4.5 ms
    LeadSpace --> Repeat : space ≈ 2.25 ms
    Repeat --> Idle : push(last_code, repeat)
    Bits --> Bits : bit (562 µs mark + 562/1687 µs space)
    Bits --> Idle : 32 bits → push(code) · bad timing → error++
```

### 6.2 Estimation (`domain/estimation/`, pure)
```mermaid
classDiagram
    direction TB
    class ImuCalibration { <<from Settings>> +int16 gyro_bias[3] +float level_pitch +float level_roll }
    class AttitudeEstimator { -float pitch_, roll_ +float alpha +update(ImuRaw, dt) Attitude }
    class YawIntegrator { -float yaw_deg_ -float bias_dps_ -uint16 still_ms_ +update(gz_raw, dt, stationary) float +rateDps() float +reset(deg) }
    class BumpDetector { -float a_lp_ +update(ImuRaw, driving) BumpResult }
    class LiftDetector { -LiftState state_ +update(ImuRaw, Attitude, TrackSpeeds, PwmRequest) LiftResult }
    class UltrasonicFilter { -uint16 last_[3] +update(EchoState, echo_us, now) Range }
    class BatteryMonitor { -float v_ema_ -float v_idle_, v_load_ -BattState state_ +update(mv, driving, dt) Battery }
    class Attitude { <<struct>> +float pitch_deg +float roll_deg +float accel_g }
    YawIntegrator ..> ImuCalibration
    AttitudeEstimator ..> ImuCalibration
    AttitudeEstimator ..> Attitude
    LiftDetector ..> Attitude
```
- Attitude: complementary filter (α = 0.98) minus level offsets.
- Yaw: integrate `gz − bias`; **auto bias re-estimation** when not driven, encoders still, |rates| < 0.5 °/s for 5 s.
- Bump: |a| minus its low-pass; spike > `bump_g` → bump + direction (x-axis sign).
- Lift: any of vertical transient while not driving · sudden tilt change · (encoders) free-spin vs calibrated on-ground speed; landed = level + steady 1 g for 1 s.
- Ultrasonic: median of 3; Timeout = no obstacle in range; closing speed over 50 ms.
- Battery: EMA volts; sag = idle − loaded; low/critical need 2 s sustained.

### 6.3 Camera head (`domain/camera/`)
```mermaid
classDiagram
    class CameraHead {
      -HeadMode mode_
      -ServoProfile pan_, tilt_
      -uint32 joy_release_ms_
      +joy(int8 x, int8 y, bool active, now)
      +set(float pan, float tilt, float speed)
      +nudge(float dpan, float dtilt)
      +home()
      +lock(bool on)
      +update(now, dt) ServoOut
    }
    class ServoProfile { -float pos_, vel_, target_ +moveTo(deg, max_speed, accel) +update(dt) float +moving() bool }
    class ServoLimits { <<from Settings>> +pan_min/max +tilt_min/max +trim_pan +trim_tilt +idle_detach }
    CameraHead *-- ServoProfile
    CameraHead ..> ServoLimits
```
```mermaid
stateDiagram-v2
    [*] --> Home
    Home --> Joy : joy active
    Joy --> Joy : stick moves
    Joy --> Home : released + return delay (unless locked)
    Joy --> Locked : lock on
    Locked --> Home : lock off
    Home --> Hold : set / nudge
    Joy --> Hold : set / nudge
    Hold --> Home : home
    Hold --> Joy : joy active
```
Trapezoidal motion at 50 Hz in µs; limits + trim applied last; optional idle detach at home.

### 6.4 Sound & LED (`domain/ui/`)
```mermaid
classDiagram
    class SoundSequencer {
      -Layer layers_[4]
      -AudioMode mode_
      +trigger(SoundId)
      +triggerCustom(const Note*, uint8 n)
      +stopRepeating(SoundId)
      +update(now) ToneOut
    }
    class LedSequencer {
      -Layer layers_[4]
      -Rgb base_
      +trigger(PatternId)
      +custom(LedMode, Rgb, period, duration)
      +setOwner(Owner)
      +update(now) LedOut
    }
    class SoundCatalogue { <<PROGMEM>> id → priority, notes[], repeat_ms }
    class LedCatalogue { <<PROGMEM>> id → priority, mode, colours[], period_ms }
    class Note { <<struct>> +uint16 hz +uint16 ms }
    SoundSequencer ..> SoundCatalogue
    SoundSequencer ..> Note
    LedSequencer ..> LedCatalogue
```
- Highest active layer plays; lower layers pause and resume (not lost).
- Audio mode: Mute → priority 1 only; Quiet → 1–2; Normal → all.
- Catalogues = tables in Spec 2 §A9; new sound/pattern = new table row (OCP).
- LED written only on colour change.
