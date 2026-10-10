# Uno Firmware — Low-Level Design

Status: IN PROGRESS · Date: 2026-10-10 · Implements: Spec 2 Part A, Spec 1 §8 · Standards: `docs/standards/coding-standards.md` §2

Sections: 1 Structure & execution ✅ · 2 Ports ✅ · 3 Motion control ✅ · 4 Safety & arbitration · 5 Comms · 6 Sensors & drivers · 7 Settings, calibration, self-test · 8 Memory budget & error handling

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
