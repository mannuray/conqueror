# Uno Firmware — Low-Level Design

Status: COMPLETE — Revision 2 (review fixes in §9 are authoritative where earlier sections differ) · Date: 2026-10-10 · Implements: Spec 2 Part A, Spec 1 §8 · Standards: `docs/standards/coding-standards.md` §2

Sections: 1 Structure & execution ✅ · 2 Ports ✅ · 3 Motion control ✅ · 4 Safety & arbitration ✅ · 5 Comms ✅ · 6 Sensors & drivers ✅ · 7 Settings, calibration, self-test ✅ · 8 Memory budget & error handling ✅ · 9 Revision 2 review fixes ✅

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
| motor | −255..255 (sign = direction); **0 = brake** (PH/EN mode, §9.14); takes `PwmOutput` only | < 20 µs | **only ControlTask includes it** | — |
| encoders | counts (sign from commanded direction), last pulse period µs | atomic snapshot (IRQs briefly off) | counting in ISRs (D3 INT1, A1 PCINT1) | disabled → zeros |
| imu | raw MPU6050 counts (±250 °/s, ±2 g) | ~0.4 ms I2C with timeout | main loop | BusError/NotFound, last values kept |
| ultrasonic | echo µs (cm = µs/58) | `trigger()` 10 µs pulse; echo timed by ISR; `poll()` non-blocking | echo edge in PCINT0 ISR | Timeout after 25 ms (no obstacle in range) |
| servos | 500–2500 µs | instant (Servo lib, Timer1) | main loop | — |
| tone | 31–20000 Hz | non-blocking (Timer2) | main loop | — |
| led | 0–255 per channel | ~30 µs IRQs off; call only on change | main loop | — |
| ir | NEC 32-bit code + repeat | decoded in PCINT0 ISR; 4-entry queue | queue ISR-safe | malformed dropped + counted |
| battery_adc | mV (divider scale applied) | ~120 µs ADC | main loop | — |
| button | debounced 20 ms | O(1) | main loop | — |
| link | bytes; 64 B RX ring, 64 B TX; **250000 baud** | non-blocking; `write` false if no room | serial ISRs in core | write false → frame dropped + counted |
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
Frames per Spec 1 §8: `0xAA | len | type | seq | payload | CRC-8`, **250000 baud** (see §9.18).

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
    class UartLink { <<driver: link>> Serial 250000 · 64/64 B }
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

---

## 7. Settings, calibration & self-test (APPROVED)

```mermaid
classDiagram
    direction TB
    class Settings {
      -GlobalSettings global_
      -SurfaceProfile active_
      -uint8 active_index_
      -bool dirty_
      +get(SettingId) int32
      +set(SettingId, int32) Result
      +activateProfile(uint8) Result
      +save() Result
      +loadOrDefault() LoadReport
    }
    class SettingDescriptor {
      <<PROGMEM table>>
      +SettingId id
      +Scope scope
      +uint8 offset
      +Type type
      +int32 min, max, def
    }
    class GlobalSettings { <<packed ~120 B>> safety thresholds · servo limits/trim · imu calib · battery scale · encoder cpr/cm · ultrasonic offset · handback · audio prefs }
    class SurfaceProfile { <<packed ~80 B>> name[8] · deadband start/keep ×4 · max_cps ×2 · track_width_eff · turn curve · PID gains ×4 · roughness signature }
    class EepromLayout { <<layout>> Header magic CQ · layout_version · active_profile · Block Global + CRC16 · Block Profile 0..2 + CRC16 each }
    class CalibrationRunner {
      -CalState state_
      -CalKind kind_
      -CalContext ctx_
      +start(CalKind, uint8 profile) Result
      +input(int32 value) Result
      +accept() Result
      +retry()
      +cancel()
      +update(CalInputs, dt) CalStep
    }
    class CalStep { <<struct>> +MotionCommand motion +uint8 progress +bool needs_input +bool done +int32 values[4] +ErrorCode error }
    class SelfTest { +run(SelfTestInputs) SelfTestResult }
    class SelfTestResult { <<struct>> +uint16 failures +bool critical +ResetReason reset }
    Settings ..> SettingDescriptor
    Settings *-- GlobalSettings
    Settings *-- SurfaceProfile
    Settings ..> EepromLayout
    CalibrationRunner ..> CalStep
    CalibrationRunner ..> Settings
    SelfTest ..> SelfTestResult
```
- Only the **active** surface profile in RAM; others stay in EEPROM.
- **CRC per block** → a corrupted block resets alone.
- `SETTING_SET/GET` generic via the descriptor table (range-checked); new setting = table row + struct field.
- EEPROM save **deferred until stopped**; `update()` writes changed bytes only.
- Layout version mismatch → matching blocks kept, others defaulted → `settings_reset` + calibration "missing".
- EEPROM use ≈ 450 / 1024 B.

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Prepare : start(kind)
    Prepare --> Running : preconditions ok (stopped, battery ok, level)
    Prepare --> Failed : precondition failed
    Running --> AwaitInput : needs measurement
    AwaitInput --> Running : input(value)
    Running --> Result : routine done
    Running --> Failed : fault / timeout / IR / STOP
    Result --> Saved : accept → Settings (+ profile)
    Result --> Running : retry
    Failed --> Idle
    Saved --> Idle
    Running --> Idle : cancel (nothing saved)
```
- Calibration motion = local **Calibration** source in the Arbitrator, below IR (IR/STOP abort); safety supervisor fully active; no deadman.
- 12 routines (CalKind, VARIANT POINT): gyro bias · level · battery divider · deadband · encoder cpr · distance · track balance · track width · turn-rate curve · PID auto-tune (relay) · servo trim · ultrasonic offset; each a step function returning `CalStep`, streamed as `CAL_RESULT`.
- **Self-test (~2 s at boot):** IMU WHO_AM_I + not saturated · battery plausible · ultrasonic echo · encoders quiet at rest · settings CRCs · reset reason. Critical (IMU, battery read) → `Locked`; non-critical → warning + degraded feature; result event + startup sound/LED.

---

## 8. Memory budget & error handling (APPROVED)

| RAM item | Bytes |
|---|---|
| Arduino core + Serial (64+64) | ~160 |
| Wire buffers | ~100 |
| Servo lib | ~30 |
| Settings (global + active profile) | ~200 |
| Controllers (5 PIDs) + motion | ~180 |
| Estimation, safety, arbitration | ~110 |
| Camera head + UI sequencers | ~80 |
| Comms (parser, event queue, writer) | ~140 |
| Scheduler, ISR state, calibration, misc | ~220 |
| Stack reserve | 400 |
| **Total ≈ 1620 B → ~430 B headroom** | |

Flash (~31.5 KB usable): core/Serial ~2.5 K · Wire ~2 K · Servo ~1 K · float math ~3 K · tone ~0.5 K · PROGMEM tables ~1.5 K · own code ~14–18 K → **~25–28 K**.
- `-Os` + LTO; no printf / float printing.
- **Build gates:** fail if static RAM > 1650 B or flash > 30 KB.
- First milestone: skeleton with all drivers, measured.
- Overflow plan (in order): shrink buffers → floats to fixed-point → drop PID auto-tune.

| Category | Example | Behaviour |
|---|---|---|
| Recoverable | bad frame, malformed IR | count, ignore |
| Degraded | encoder fault | OpenLoop + `encoder_fault` |
| Degraded | ultrasonic dead | obstacle brake unavailable flag; speed cap 50 % |
| Degraded | IMU bus error mid-run | retry 3× → `imu_fault`: heading hold off, speed cap 30 %, tilt/lift safety lost → warning sound + flag |
| Critical at boot | IMU missing, battery implausible | `Locked` |
| Fatal | hung task | watchdog reset, reason at next boot |
- `Status`/`Result` enums, no exceptions; `CQ_ASSERT` debug → safe stop + error event, release → counter.
- All error codes from the shared `protocol/errors` registry.

---

## 9. Revision 2 — review fixes (APPROVED decisions; authoritative where earlier sections differ)
Source: independent review (26 findings) + ELEGOO shield schematic `ConquerorCar-Shield-V1.0.pdf`. User decisions: **UART 250000 baud**; **calibration maths moved to the ESP32**.

### 9.1 Hardware facts confirmed from the shield schematic
- **DRV8835 MODE pin tied to VCC → PH/EN mode.** AIN1/PH ← D8, AIN2/EN ← D5 (right); BIN1/PH ← D7, BIN2/EN ← D6 (left). **EN = 0 brakes (slow decay); coast is impossible.**
- **Upload switch S1** on the shield disconnects RX from the ESP32 header for USB uploads.
- Old line-sensor header P7 = A0, A1, A2 + **5 V + GND** → buzzer + right encoder power available there; left encoder A uses **D3 on header P3**.
- Battery divider R1 10 kΩ / R4 1.5 kΩ → scale (10 + 1.5) / 1.5 = 7.667.
- Mode button S2 on D2 with 10 kΩ pull-up; IR on D9; WS2812 on D4 (100 kΩ pull-down); servos D10/D11; ultrasonic D13/D12; MPU6050 on A4/A5 (AD0 unconnected → 0x68).

### 9.2 Link, control-loss and deadman rules (fixes 1, 2)
- **Link loss stops only network-sourced motion.** With owner = **Ir**, link state is ignored (IR works with no ESP32). With owner = **Network** or **Calibration** (started from the network): link lost → SoftStop.
- **Deadman applies only in Primitives `Teleop` with owner = Network** (streamed `drive`/`drive_rate`). TURN/MOVE/IR/calibration are not subject to the 300 ms deadman.
- **On link loss:** `Primitives.cancel()` → `primitive_done{ok:false, reason:LINK_LOST}`; calibration aborted; **nothing resumes** — a new command is required (Spec 0 principle 5).
```mermaid
stateDiagram-v2
    [*] --> Linked
    Linked --> Lost : no valid frame 300 ms
    Lost --> Linked : valid frame
    state Lost {
        [*] --> CheckOwner
        CheckOwner --> SoftStopAndCancel : owner = Network or Calibration
        CheckOwner --> NoEffect : owner = Ir or None
    }
```

### 9.3 Non-blocking EEPROM (fix 3)
- New `app/EepromWriter`: queue of dirty ranges; writes **one byte per scheduler pass** by polling `EECR.EEPE` (never `EEPROM.update` busy-wait); **block CRC written last**; emits `settings_saved` when done.
- Boot with invalid/old layout: defaults applied **in RAM only**; persisted lazily by the writer (never synchronously at boot → no watchdog reset loop).
- Port change: `eeprom::read(...)` + `eeprom::writeByteAsync(addr, value)` + `eeprom::busy()`.

### 9.4 Boot handshake & envelope sync (fix 4, 26)
```mermaid
sequenceDiagram
    participant U as Uno
    participant E as ESP32
    U->>U: .init0 read reset reason (r2 / .noinit marker) · wdt_disable in .init3
    U->>U: load settings (RAM) · self-test (kicks WDT) · WiFi-reset window
    loop every 500 ms until HELLO_ACK
        U->>E: HELLO {fw, proto, caps, reset_reason}
    end
    E->>U: ENVELOPE (from NVS, if any)
    E->>U: HELLO_ACK {proto}
    U->>U: synced = true
    Note over U: before sync: network motion → BUSY; IR allowed
    E->>U: HELLO_REQ (any time) → Uno re-sends HELLO
```
New frames: `0x02 HELLO_ACK {proto u8}` and `0x03 HELLO_REQ {}` (ESP32 → Uno).

### 9.5 Caps applied to targets, not PWM (fix 5)
New pure class **`TargetLimiter`** between Mixer and Ramp:
```
Mixer → TargetLimiter(obstacle scale on forward v, envelope speed_cap, degraded caps) → Ramp → SpeedController
```
- Works in **% of max speed** (so child-play 20 % really is 20 % speed).
- Sets `capped` → `SpeedController` **freezes PID integrators** while capped (no wind-up lurch).
- `SafetySupervisor.gate()` keeps only hard stops (Locked, Halted, SoftStop, stall) + a **final PWM clamp backstop**.
- Degraded caps: ultrasonic fault → 50 %; IMU fault → 30 % (added to Spec 2 A11 + flags).

### 9.6 Encoder fault vs stall (fix 6, 7)
| Condition (track driven above deadband for 0.5 s, no counts) | Other evidence | Verdict |
|---|---|---|
| gyro or other encoder shows the tank moving consistently with this track | yes | **encoder fault** → latch until reboot / re-enable; OpenLoop + `encoder_fault` |
| no gyro motion and other track not moving / also stuck | no | **stall** → safety cuts that track (clears on direction change or 1 s) |
- **Both encoders count rising edges only:** INT1 rising (D3); `PCINT1_vect` reads PINC, detects the **PC1 rising edge** (A1) and ignores falling edges.

### 9.7 Calibration on the ESP32 (fix 8, 19; user decision)
- The Uno runs only **calibration motion scripts** and streams raw samples; the **ESP32 computes results** and writes them back with `SETTING_SET` + `SETTINGS_SAVE`.
- `CalibrationRunner` → renamed **`CalScriptRunner`**: scripts = closed set (VARIANT POINT): `StillSample` (gyro bias, level) · `PwmRamp(track, dir)` (deadband) · `FullSpin` (balance) · `SpinAndArc` (track width) · `RateSteps` (turn curve) · `RelayOscillation` (PID auto-tune) · `DriveCounts(n)` (distance) · `ServoNudge` · `Idle` (battery divider / ultrasonic offset / encoder CPR: input-only, ESP32 asks the user).
- Wire: `CALIBRATE {op: start|cancel, script u8, param i32}`; Uno → ESP32 **`CAL_DATA` (0x95) {script, phase, t_ms u16, v[8] i16}** at 50 Hz (gyro rate, accel, encoder counts, PWM); `CAL_RESULT` reused for script done/fail.
- **`Owner::Calibration`** added to the Arbitrator: below IR (IR/STOP abort), above Network (network motion → BUSY while calibrating). Calibration **refused while an envelope is active**.
- IR `#` hold 3 s → `cal_request` event to the ESP32 (no ESP32 → error beep).
- Calibration **status bits** (missing/ok per item, 2 bits × 12) stored on the Uno; **"stale"** decided by the ESP32.

### 9.8 IR remote full design (fix 9)
| Key (remote A NEC code) | Action | Hold semantics |
|---|---|---|
| ▲ `0x00FF629D` / ▼ `0x00FFA857` | Drive v = ±ir_speed, w = 0 (heading hold) | repeats keep alive; stop 200 ms after last repeat |
| ◀ `0x00FF22DD` / ▶ `0x00FFC23D` | Spin w = ±ir_speed | same |
| OK `0x00FF02FD` | STOP + release to None | — |
| 1 `0x00FF6897` / 3 `0x00FFB04F` | `CameraHead::jog(pan ∓rate)` | while held |
| 2 `0x00FF9867` / 8 `0x00FF4AB5` | `jog(tilt ±rate)` | while held |
| 5 `0x00FF38C7` | camera home | — |
| 4 `0x00FF30CF` / 6 `0x00FF7A85` | ir_speed −/+ 10 % (20–100, default 50), beep pitch ∝ speed | — |
| 7 `0x00FF10EF` | mute toggle | — |
| 9 `0x00FF52AD` | battery check: LED colour + 3/2/1 beeps | — |
| 0 | gyro TURN by 180° | — |
| * | parking beeps toggle | — |
| # | (hold 3 s) → `cal_request` to ESP32 | hold timer |
Codes are the 32-bit values as shifted in by the decoder in received order (matches the original firmware constants); 0/*/# codes captured during hardware bring-up. `IrKeymap` in PROGMEM; `ir_speed_pct` held by the Arbitrator. New `CameraHead::jog(pan_rate, tilt_rate)`.

### 9.9 Annunciator — when sounds and LED patterns play (fix 10)
New pure class **`Annunciator`**: a PROGMEM table **condition → (SoundId, PatternId, priority, continuous?)** evaluated at 50 Hz from flags/events (tilted, lifted, batt_low/critical, bump, stall, link lost, reversing, start forward, turning, owner, calibrating, startup/ready/selftest fail).
- Continuous conditions keep a layer active; parking beeps use a **parameterised repeat interval** = f(distance).
- Owner colour: Ir (local) = purple; Network owner colour from **OWNER_HINT** (human = green, AI = cyan).
- **TONE notes copied into an owned buffer** (10 × 4 B) — never pointing into the frame buffer.

### 9.10 WiFi-reset button (fix 11)
`app/WifiResetDetector`: active only in the boot window; if D2 is held at boot and stays held for **5 s** → `WIFI_RESET_REQ` (0x96) sent, **re-sent every 500 ms until acked** (ACK frame), confirmation sound + LED; one-shot per boot.

### 9.11 Telemetry encoding (fix 12)
TELEM_FAST (≈ 46 B):
| Field | Wire | Scale / sentinel |
|---|---|---|
| uno_ms | u32 | ms |
| yaw | u16 | 0.01°, 0–35999 (CCW positive) |
| yaw_rate | i16 | 0.1 °/s |
| pitch, roll | i16 | 0.01° |
| spd_cmd L/R | i8 | % |
| spd_act L/R | i8 | %, **−128 = n/a** (no encoders) |
| pwm L/R | i16 | −255..255 |
| dist_mm | u16 | mm, **0xFFFF = clear (no obstacle)**, **0xFFFE = sensor fault** |
| closing_cm_s | i16 | cm/s (for ESP32 TTC) |
| pan, tilt | i16 | 0.1° |
| enc L/R | i32 | counts |
| flags | u16 | bit0 blocked · 1 lifted · 2 tilted · 3 stall_l · 4 stall_r · 5 cam_moving · 6 turn_limited · 7 capped · 8 soft_stop · 9 halted · 10 locked · 11 imu_fault · 12 us_fault · 13 encoder_fault · 14 calibrating · 15 synced |
| prim, prim_pct | u8, u8 | primitive kind, % |

TELEM_SLOW (≈ 40 B): uptime_s u32 · reset_reason u8 · batt_mv u16 · driving u8 (for ESP32 sag) · temp i16 (0.01 °C) · owner u8 · flags2 u32 (batt_low, batt_crit, link_lost, brake_off, muted, quiet, envelope, deadman, …) · accel_var u16 · loop_load u8 · loop_max_us u16 · free_ram u16 · err_frames u16 · err_i2c u16 · settings_crc u16 · envelope u8×3 · **profile_idx u8** · **cal_status 3 B** · **hw u8** (encoders_enabled, imu_ok, us_ok).
- New pure **`AccelVariance`** estimator (IMU task) → `accel_var`. Sag computed by the ESP32 from batt_mv + driving.
- **Sign convention:** yaw and ω are **CCW-positive**; `w > 0` turns left. Clients map "stick right" to `w < 0`.

### 9.12 Layering fixes (fix 13, 25)
- Value types (`EncoderSnapshot`, `ImuRaw`, `IrEvent`, `Rgb`, `ResetReason`, …) move to **`src/types/`**, shared by ports and domain; domain still never includes `ports/`.
- `util/progmem.h` shim: PROGMEM macros on AVR, plain const on host.
- **Safety by type:** `motor::write(const PwmOutput&)`; `PwmOutput` has a private constructor, `friend class SafetySupervisor` → nothing else can produce motor commands.
- `HeadingController::update(const HeadingInputs&)` (parameter struct; ≤ 4 params rule).
- Naming: diagrams use `name_`; **code uses the standard's `m_` prefix**.

### 9.13 Timing exceptions & reset reason (fix 14, 15, 22, 24)
- **Documented exception:** the MPU6050 burst read (~380 µs at 400 kHz) may exceed the 50 µs busy-wait rule; Wire timeout via `setWireTimeout(3000, true)` (pin AVR core ≥ 1.8.3) + **I2C bus recovery** (9 SCL clocks) on timeout.
- Battery ADC: **one sample per tick** (10 Hz), averaged over 4 ticks.
- Pitch/roll (atan2/sqrt) computed at **50 Hz**; gyro integration stays at 200 Hz.
- Reset reason: read **r2 in `.init0`** (Optiboot ≥ 6) or a **`.noinit` marker** set before intentional WDT resets; `wdt_disable` in `.init3`; may report `Unknown` with old bootloaders (documented).
- Self-test kicks the WDT between steps.
- Safety supervisor runs at **100 Hz** (Spec 2 A2 updated).
- **WS2812** writes rate-limited to ≤ 20 Hz and on-change; accepted servo jitter ≤ ~2.7° transient.
- `MOTOR_PWM_244HZ` build option: the clock port returns corrected µs/ms, so ISRs (NEC, echo) stay correct.

### 9.14 Motor port contract (fix 16)
`motor::write(PwmOutput)`: −255..255; **0 = brake (slow decay)** — PH/EN mode, coast impossible. `StopMode::Smooth` = ramp to 0 over ~300 ms; `Brake` = immediate 0.

### 9.15 Lift detection (fix 17)
Lifted if **any**: sustained vertical acceleration deviation > 0.25 g for > 150 ms (**regardless of driving**) · |a| < 0.6 g (falling/held) · sudden tilt change > 15° within 300 ms · (encoders) free-spin. **Bumps rejected** by duration (< 50 ms spikes) and shape. Landed = level + steady 1 g for 1 s.

### 9.16 Ultrasonic fault handling (fix 18)
`EchoState {Pending, Ready, Clear, NoEcho}`:
- **No rising edge** within 2 ms of trigger → `NoEcho` (sensor fault). 3 consecutive → `us_fault` → obstacle brake **unavailable** → degraded cap 50 % + flag + warning.
- Echo high > 25 ms → `Clear` (nothing in range).
- **No new trigger while echo is still high** (clone modules hold up to 200 ms).
- Self-test: fault only on `NoEcho`, not on `Clear` (open rooms pass).

### 9.17 Settings & profile ownership (fix 20, 21)
- **SettingDescriptor table** (PROGMEM): `id u8 · scope (Global/Profile) · type · unit · fixed-point scale (e.g. batt_low in mV, bump in mg, gains ×1000) · min · max · default`.
- **Profile names live on the ESP32** (NVS); the Uno stores profiles by index 0–2 only.
- Roughness signature and battery sag: **ESP32 only** (removed from the Uno).
- `Envelope.apply`: setting `lock_brake = 1` **forces the obstacle brake on**; Uno rejects `BRAKE off` while locked (defence in depth); who may loosen = ESP32 role check.
- PIDs **reference gains in the active profile** (no duplicated gain copies).

### 9.18 Comms additions (fix 23, 26)
- **UART 250000 baud** (0 % error at 16 MHz with U2X; supported by the ESP32).
- Parser buffer **is** the frame (dispatch by reference; no duplicate `Frame.payload`); one shared 52 B TX encode buffer.
- TxScheduler priority: **ACK > WIFI_RESET_REQ > EVENT > HELLO > CAL_DATA > TELEM_FAST > TELEM_SLOW**.
- **NOT_OWNER / BUSY rejection events rate-limited** to 1 per 500 ms per source (a 20 Hz DRIVE stream cannot flood the queue).
- **Uno owner `Network → None`:** no network motion command for 1 s and Primitives idle (bookkeeping only; human/AI handback is the ESP32's).
- **EventKind → protocol event mapping** (generated table): e.g. `primitive_done a = kind | ok<<8, b = reason code`; `selftest a = failure bitmask, b = critical`; `bump a = mg, b = direction`.

### 9.19 Revised control tick sequence
```mermaid
sequenceDiagram
    participant C as ControlTask
    participant A as Arbitrator
    participant P as Primitives
    participant H as HeadingController
    participant X as Mixer
    participant L as TargetLimiter
    participant R as Ramp
    participant ES as EncoderSpeed
    participant V as SpeedController
    participant F as SafetySupervisor
    C->>ES: update(encoder snapshot, cmd signs)
    C->>A: update(now, link, inputs)
    A-->>C: command + owner
    C->>P: update(command, yaw, travelled)
    P-->>C: MotionTarget
    C->>H: update(HeadingInputs)
    H-->>C: Δ
    C->>X: mix(v, Δ)
    X-->>C: track targets %
    C->>L: limit(targets, obstacle, envelope, degraded)
    L-->>C: limited targets + capped
    C->>R: apply(targets, accel cap)
    C->>V: update(targets, speeds, batt, capped)
    V-->>C: PwmRequest
    C->>F: gate(request, safety inputs)
    F-->>C: PwmOutput (only constructible here)
    C->>C: motor::write(PwmOutput)
```

### 9.20 Revised memory budget
| RAM item | Bytes |
|---|---|
| Core + Serial (64+64) | ~160 |
| Wire (5 × 32) | ~160 |
| Servo | ~30 |
| Settings (global + active profile, gains referenced) | ~200 |
| Controllers + TargetLimiter + motion | ~170 |
| Estimation (+ AccelVariance), safety, arbitration | ~130 |
| Camera head, UI sequencers, Annunciator, TONE buffer (41) | ~140 |
| Comms (parser = frame, TX buffer 52, event queue 8 × 6) | ~160 |
| Scheduler, ISR state, EepromWriter queue, CalScriptRunner, misc | ~190 |
| Stack reserve | 400 |
| **Total ≈ 1740 B → ~300 B headroom** | |
Flash: calibration maths moved to the ESP32 saves an estimated 3–4 K → own code ~12–15 K, total ~23–26 K. Build gates unchanged (RAM ≤ 1750 B static, flash ≤ 30 KB).

### 9.21 Alignment with ESP32 LLD Revision 2
- `CALIBRATE` carries `profile u8` (as in Spec 1).
- `WifiResetDetector` → **`BootButtonDetector`**: at boot, held 5 s → `WIFI_RESET_REQ` (0x96); held **15 s** → `ADMIN_RESET_REQ` (0x97); both resent every 500 ms until ACK; distinct confirmation sounds/LED.
- After the boot window, a short press of D2 → event **`button_press`** (used by the ESP32 for first-run physical confirmation).
- New error reasons/events from the ESP32 revision are added to the shared registry.

### Hardware revision (2026-10-11): Smart Car Shield V1.1 — dual-shield support (authoritative)
User confirmed the Uno shield is **ELEGOO SmartCar Shield V1.1** (Smart Robot Car V4 family); the ESP32 board is **V1.0** (no IO13 LED). **Motor driver chip confirmed by the user: TB6612FNG (2026-10-11).** Shield probe in bench bring-up still verifies the pin map (D3 standby, direction pins).

| Function | **SHIELD_SMARTCAR_V11_TB6612 (default)** | SHIELD_TANK_DRV8835 |
|---|---|---|
| Driver | TB6612FNG | DRV8835 (PH/EN, MODE high) |
| PWM right (A) / left (B) | D5 / D6 | D5 / D6 |
| Direction A / B | **D7 / D8** | D8 / D7 |
| Standby | **D3 (HIGH = enabled; LOW = coast)** | — |
| PWM 0 | short brake | brake |
| Coast available | yes (STBY low) | no |
Source for the V1.1 pin map: ELEGOO V4 driver code (third-party copy `raccrobotics/libelegoo-edu`); verify against the board.

**Pin plan change (both shields):** D3 is no longer free → **left encoder A on A1, right encoder A on A2** (both PCINT1, rising edges via PINC diff), **buzzer on A0**; the old line-sensor header (A0–A2 + 5 V + GND) carries buzzer + both encoders. D3 = motor standby (TB6612) or unused (DRV8835).
Firmware: `drivers/board_config.h` build option selects the shield; only the motor driver class + pin map differ; `StopMode::Coast` enabled when the shield supports it.

### Decisions (2026-10-11): motor driver config + Uno OTA
- ELEGOO ships the SmartCar Shield V1.1 with **either TB6612FNG or DRV8835** (TB6612 used during the DRV shortage). **Both supported via a build option** (`SHIELD_SMARTCAR_V11_TB6612` default for this tank, `SHIELD_SMARTCAR_V11_DRV8835` / `SHIELD_TANK_DRV8835`); flash the firmware matching the chip. The bench shield probe still verifies the pin map and warns if the build doesn't match the board (motors only run with D3 HIGH → TB6612).
- **Uno updates stay USB-only** (shield switch S1). Reason recorded: the ESP32↔Uno connector P1 carries only RX(IO33)/TX(IO4)/GND/VCC — no reset line, so the ESP32 cannot put the Uno into its bootloader. Future option (not planned): one wire ESP32 pad T14 (IO13) → 100 nF → Uno RESET + STK500 flashing from the ESP32 (esp-link approach).
- ESP32 self-OTA unchanged (dual partitions + rollback).
- 2022.03.23 ELEGOO package reviewed: its ESP32-WROVER-Camera-V1.2 schematic is identical to the one already used.

### Decision (2026-10-11): mounting and compass
- Compass dropped (no magnetometer support anywhere). IMU = MPU6050 on the shield socket.
- Stock baseplate mount; IMU axis mapping constant in `board_config.h`, measured at bring-up; bench mount check verifies (gravity → up axis, hand rotation → yaw sign).

### Decision note (2026-10-11): frame parser resync (see §5.2)
`FrameParser` must resync like the Plan A reference (`conqueror_protocol.framing`): when a length byte is out of range and is itself 0xAA, treat it as a new start of frame; after a CRC mismatch, rescan the bytes received after the false start for the next 0xAA, so noise or a cut-off frame does not cost the next good frame.
