# Spec 06 — Base board (RP2354B) + camera head

Status: **draft for review** (2026-10-14). Requirements agreed with the owner in `docs/design-notes.md` §40 / §40.1; this spec turns them into a hardware design. **Revision 1 (2026-10-27, design-notes §159): arm-ready analog** — servo position feedback on every servo header, servo current sense, VBUS on a digital pin, motor current sense dropped (§12). Next steps after approval: LLD (schematic-level: exact parts, final pin map, layout rules), then a plan.

**Scope:** a new product, separate from the current tank. The current development — Mega 2560 + ELEGOO SmartCar Shield + ESP32-S3 bridge (Plans B/C/D) — continues unchanged. The base board later reuses that firmware's design (protocol, domain logic) with a new driver layer.

---

## 1. Goal

A **good general-purpose robot controller** for tracked and wheeled robots, sold **worldwide**. One board replaces "Uno/Mega + motor shield"; an off-the-shelf ESP32-S3 camera board on the pan-tilt gimbal adds WiFi, video and phone driving. The value is the integration (encoders, IMU, safety, camera head) and the software, not the lowest price.

**Non-goals:** Arduino shield compatibility · battery charging on the board · Python on the board (Tank Logo is the beginner language) · Grove connectors · a custom camera board.

## 2. System overview

```mermaid
flowchart LR
  subgraph HEAD["Camera head (off-the-shelf ESP32-S3 camera board, on the gimbal)"]
    ESP[ESP32-S3 + PSRAM] --- CAM[OV2640/OV3660]
    ESP --- WIFI((WiFi: phone page,<br/>bench, OTA))
  end
  subgraph BASE["Base board (Uno outline, RP2354B)"]
    MCU[RP2354B, 2 MB flash inside]
    MCU --- DRV[2x motor driver, 4 ch<br/>switch: MOTOR C/D or SERVO 3-6]
    MCU --- ENC[4x encoder A/B]
    MCU --- EXP[Expansion header<br/>6 GPIO + 4 ADC]
    MCU --- SRV[2 servo out (pan/tilt)<br/>+ 4 shared with motors C/D]
    MCU --- SENS[Ultrasonic · IR · RC input]
    MCU --- I2C[I2C: gyro socket · compass socket · Qwiic]
    MCU --- OUT[Buzzer · WS2812 · status LEDs]
    MCU --- UI[RESET · MODE · BOOT]
    MCU --- USB[USB-C: programming]
  end
  BAT[2S Li-ion pack<br/>own USB-C charging] --> BASE
  ADP[DC adapter 7-12 V<br/>barrel jack] --> BASE
  BASE == "4-wire harness:<br/>5V, GND, TX, RX" ==> HEAD
  DRV --> M[2 DC motors<br/>(4 in MOTOR position)]
```

Roles (unchanged from the current architecture):
- **Base = body.** Real-time control and **all safety** (deadman, link-loss stop, obstacle brake, tilt/lift, stall, low battery, child-play envelope). A crashed WiFi chip, a sleeping phone or a bad OTA can never leave the robot driving.
- **Head = gateway.** WiFi, camera, phone page, bench passthrough, OTA. Master of **communication only**.
- Link: the existing body-link protocol (binary frames, CRC-8, protocol version 2) over UART.

## 3. Base board requirements

| Group | Requirement |
|---|---|
| Size | Uno outline 68.6 x 53.3 mm, the 4 Uno mounting holes. Not shield-compatible |
| MCU | **RP2354B** (owner, 2026-10-14): RP2350B (QFN-80, **48 GPIO**, 8 ADC) with **2 MB flash inside**, A4 stepping (erratum E9 fixed), 12 MHz crystal. Only US$0.10 more than the A package; LCSC C39843328 in stock (2026-10-14). Fallback: RP2350B + external QSPI flash |
| Motors | **4 channels on every board** (2x TB6612-class, ≥ 1.2 A continuous / 3 A peak per channel, VM up to 12 V). Motors A/B always; motors C/D or servos 3-6 chosen by the **MOTOR C/D ⇄ SERVO 3-6 switch** (owner, 2026-10-14) |
| Encoders | **4 connectors** (one per motor), quadrature A/B, 5 V supply |
| Servos | **2 dedicated** (pan/tilt) **+ 4 shared** with motors C/D (SERVO position) = **6 servos**; more than 6 via a PCA9685 module on Qwiic. Servo power from a **dedicated servo buck** with jumpers **5 V / 6 V / BATT** (BATT = HV servos on a 2S pack only). Servos only on the base (owner, 2026-10-14: servo pins on the head dropped). Servos run on the RP2354B's **own hardware PWM** (~0.3 µs steps ≈ 0.03°), not an on-board PCA9685 (~4.9 µs ≈ 0.45°; owner, 2026-10-27). **Servo headers are 4-pin: signal, V+, GND, feedback** (Rev 1, §12) — a normal 3-pin plug fits pins 1-3; a servo's pot wire (the feedback mod) goes on pin 4 |
| Distance | HC-SR04 connector (5 V, trig, echo) |
| IR | IR receiver at the front edge + a connector for an off-board receiver |
| RC input | 1 connector: 5 V, GND, 2 signal pins — hobby 2.4 GHz receivers (PPM, SBUS) or a toy 27 MHz decoder's on/off outputs |
| IMU | Socket for a **GY-521** breakout (MPU6050 class, 8 pins: VCC GND SCL SDA XDA XCL AD0 INT) |
| Compass (optional) | Socket for a **GY-271** breakout (QMC5883L/HMC5883L, 5 pins: VCC GND SCL SDA DRDY) at the board edge, far from the motor connectors |
| I2C expansion | 1 **Qwiic** (JST-SH 4-pin) port — ToF, newer IMUs, a remote compass |
| Expansion header | 2.54 mm header with 5 V, 3.3 V, GND, **6 GPIO** (5 V-tolerant; UART1/I2C1/SPI capable) and **4 analog inputs** (0-9.9 V through a 1/3 divider, 5 V-safe — Rev 1) for users' own add-ons. The 4 analog inputs are the **same ADC pins as the feedback pins of servo headers 3-6**: use one or the other (§12) |
| Outputs | Passive buzzer · 1 WS2812 RGB LED (buffered data) · GPIO status LEDs |
| Buttons | RESET, MODE (short press; 5 s WiFi reset; 15 s admin reset), BOOT (recovery only, recessed). No extra user buttons |
| Sensing | Input voltage · USB-VBUS present (digital pin, Rev 1) · **servo rail current** (one shunt + amplifier, Rev 1) · **servo position feedback** (6, on the servo headers' pin 4, Rev 1) |
| Head link | 4-wire harness connector (§6) |
| USB | USB-C, programming (RP2350 native USB); **in the arm role also the data link to the laptop** (USB CDC serial; Rev 1, §12.1) |
| Power | **2S Li-ion pack** (own protection and USB-C charging; JST-XH 2-pin) **or a 7-12 V DC adapter** (barrel jack, owner 2026-10-14; the jack's switch contact disconnects the battery when plugged). Input range 6-12 V; reverse-polarity protection; TVS; fuse; main switch; 5 V logic, 5 V/6 V servo and 3.3 V rails (§5). No charger on the board |

### 3.0 One board, three robots
| Robot | Switch | Motors | Servos |
|---|---|---|---|
| Car / tank | SERVO | 2 (A/B) | up to 6 (pan/tilt + 4) |
| Mecanum / 4-motor tank | MOTOR | 4, each with an encoder | 2 |
| Arm | SERVO | 0 | 6, each with optional position feedback (+ PCA9685 on Qwiic for more, no feedback) |

**One role at a time (owner, 2026-10-27, Rev 1):** a board is a tank/car controller *or* an arm controller, not both. An arm on a tank (motors and 6 feedback servos together) is not a v1 target; with the expansion analog inputs in use as pot inputs, an arm has no free analog add-ons (no joystick).

Stationary robots (an arm on a desk) can run from a 7-12 V adapter; tracked/wheeled robots use the 2S pack (TT motors run too fast and hot at 12 V).

### 3.1 Board-wide rules
1. **5 V-safe connectors.** Users never think about voltage: every connector carries 5 V power; every input tolerates 5 V; every output works with 5 V parts. The only buffered output is the WS2812 data line (74AHCT1G125-class; WS2812 needs ~3.5 V high).
2. **Robot connectors are JST-XH 2.54 mm** (servos on standard 3-pin 2.54 mm male headers). Compact, latching, cheap, easy to crimp, sold everywhere.
3. **Enclosure rule.** Anything the user reaches or must see sits on a board edge, facing out: buttons are right-angle side-press switches. Proposed **service edge**: USB-C, main switch, RESET, MODE (one cut-out in an enclosure). IR receiver and RGB LED at edges or on connectors; status LEDs along an edge or side-view.
4. **USB never drives motors.** If USB VBUS is present while the board runs from the battery, the firmware blocks the motors (no driving off with a cable attached). The rule is about motors only: servos move with USB attached, and in the arm role USB is the normal link (Rev 1, §12.1).

## 4. Draft pin plan (RP2354B)

Draft for the LLD — the LLD finalises it against the datasheet (PWM slices, PIO assignment, 5 V tolerance per pin). On the B package the **ADC inputs are GPIO 40-47**; GPIO 26-39 are digital. **5 V signals only on GPIO 0-39**; GPIO 40-47 are treated as 3.3 V only (to be confirmed in the datasheet before the schematic).

| GPIO | Function | Electrical | Peripheral |
|---|---|---|---|
| 0 / 1 | Head UART TX / RX | 3.3 V (ESP32 is 3.3 V) | UART0 |
| 2 / 3 | Motor A IN1 / IN2 | → TB6612 #1 AIN1/AIN2 | PWM slice 1, 20 kHz |
| 4 / 5 | I2C SDA / SCL | 2.2 kΩ pull-ups to 3.3 V | I2C0, 400 kHz |
| 6 / 7 | Motor B IN1 / IN2 | → TB6612 #1 BIN1/BIN2 | PWM slice 3 |
| 8 / 9 | Motor C IN1 / IN2 **or** servo 3 / 4 | → TB6612 #2 AIN1/AIN2 **and** servo headers 3/4 (100 Ω) | PWM slice 4: 20 kHz (MOTOR) or 50 Hz (SERVO) |
| 10 / 11 | Motor D IN1 / IN2 **or** servo 5 / 6 | → TB6612 #2 BIN1/BIN2 **and** servo headers 5/6 | PWM slice 5 |
| 12 / 13 | Servo 1 (pan) / 2 (tilt) | 100 Ω series | PWM slice 6, 50 Hz |
| 14 | Buzzer | → MOSFET gate | PWM slice 7 A (tone) |
| 15 | WS2812 data | → 74AHCT2G125 A | PIO0 |
| 16 / 17 | Encoder A (left) A / B | 5 V in, 1 kΩ series, 10 kΩ pull-up to 3.3 V | PIO1 quadrature |
| 18 / 19 | Encoder B (right) A / B | same | PIO1 quadrature |
| 20 | Motor STBY | → TB6612 #1 STBY; → TB6612 #2 STBY through switch pole 1 (SERVO: held low). 100 kΩ pull-downs → motors coast at reset | GPIO |
| 21 | Ultrasonic trig | → 74AHCT2G125 B (5 V out) | GPIO |
| 22 | Ultrasonic echo | 5 V in, 1 kΩ series | PIO2 |
| 23 | IR receiver | on-board 3.3 V / off-board via 1 kΩ | PIO2 |
| 24 / 25 | RC input 1 / 2 | 5 V in, 1 kΩ series | PIO2 (PPM, SBUS via pad inversion, digital) |
| 26 / 27 | Encoder C A / B | 5 V in, 1 kΩ series, 10 kΩ pull-up | PIO1 quadrature |
| 28 / 29 | Encoder D A / B | same | PIO1 quadrature |
| 30 | MODE button | 10 kΩ pull-up to 3.3 V, 100 nF, switch to GND | GPIO |
| 31 | MOTOR/SERVO switch sense | switch pole 2 to GND, 10 kΩ pull-up | GPIO |
| 32 / 33 | Expansion (UART1 TX / RX capable) | 5 V-tolerant in, 1 kΩ series | expansion header |
| 34 / 35 | Expansion (I2C1 / SPI capable) | 5 V-tolerant in, 1 kΩ series | expansion header |
| 36 | GY-521 INT | from the socket (optional use) | GPIO |
| 37 | **USB VBUS present** (Rev 1; was GY-271 DRDY — the compass is polled, DRDY not connected) | divider 100 kΩ / 150 kΩ (5 V → 3.0 V) | GPIO |
| 38 / 39 | Expansion GPIO | 5 V-tolerant in, 1 kΩ series | expansion header |
| 40 (ADC0) | Input voltage (battery/adapter) | divider 100 kΩ / 22 kΩ + 100 nF (12 V → 2.16 V) | ADC |
| 41 (ADC1) | **Servo rail current** (Rev 1; was VBUS) | low-side shunt in the servo rail's return + current-sense amplifier (INA180-class), scaled to ~8 A full scale | ADC |
| 42-45 (ADC2-5) | Expansion analog inputs **and** servo 3-6 feedback (header pin 4) | **0-9.9 V**: 1/3 divider (20 kΩ / 10 kΩ) + 100 nF, 5 V-safe (Rev 1) | expansion header + servo headers 3-6 |
| 46 / 47 (ADC6/7) | **Servo 1 / 2 feedback** (header pin 4; Rev 1 — was reserved for motor current sense) | 0-9.9 V: 1/3 divider + 100 nF | servo headers 1-2 |

Budget: 48 GPIO — 36 used by the board, **6 GPIO + 4 ADC on the expansion header** (the 4 ADC shared with servo 3-6 feedback). All 8 ADC used: input voltage, servo current, 6 servo feedback. All 4 motors have encoders. PIO: 9 of 12 state machines (4 encoders, WS2812, echo, IR, 2 RC).

## 5. Power tree

```mermaid
flowchart TB
  PACK[2S Li-ion pack 6.0-8.4 V<br/>own protection + USB-C charging] --> CON[JST-XH 2-pin]
  ADP[DC adapter 7-12 V] --> JACK[Barrel jack<br/>switch contact cuts the battery]
  CON --> JACK
  JACK --> RP[P-FET: reverse protection<br/>+ main switch, TVS]
  RP --> FUSE[Fuse]
  FUSE --> VIN((VIN 6-12 V))
  VIN --> MD[2x motor driver VM]
  VIN --> DIV[Divider → ADC0]
  VIN --> BUCK5[5 V buck, 2-3 A]
  VIN --> SBUCK[Servo buck, ~5 A<br/>jumper 5 V / 6 V]
  BUCK5 --> V5L((5 V logic))
  SBUCK --> SJ{Servo source jumper}
  VIN --> SJ
  SJ --> V5S((Servo rail<br/>5 V / 6 V / BATT))
  V5L --> HEADV[Head harness 5 V]
  V5L --> SENS5[Sensor connectors 5 V]
  V5S --> SERVOS[6 servo headers]
  V5L --> OR{Diode-OR}
  USB[USB-C VBUS] --> OR
  OR --> LDO[3.3 V LDO]
  LDO --> V33((3.3 V: RP2350, logic))
  V33 --> QW[Qwiic port 3.3 V]
  USB --> VDIV[Divider → GPIO37]
  V5S --> SHUNT[Servo current shunt + amp → ADC1]
```

- **No charging on the board** (owner, 2026-10-14). The pack charges itself over USB-C; the main switch should be off while charging.
- **Battery or adapter, never both:** the barrel jack's switch contact disconnects the battery connector when a plug is inserted. Input range 6-12 V (motor drivers to 13.5 V, bucks to 17 V); TVS on the input.
- **USB powers only the 3.3 V logic** (RP2350, flash) through the diode-OR, so the board can be programmed at any time without a battery. USB never back-feeds the 5 V rail, the motors or the head.
- **Servo power is separate from the logic rail:** a dedicated servo buck (~5 A, 5 V or 6 V by jumper) — up to 6 servos (MG996R-class arm servos peak ~2.5 A each); servo bursts can brown out an ESP32 (design-notes §38). The **BATT** jumper position feeds the servo rail straight from VIN for **HV servos on a 2S pack only** — never with a 12 V adapter (silkscreen warning; no pin is left to detect it).
- **5 V logic buck 2-3 A:** head ESP32 + camera (~0.5 A peaks), sensors, GY-521/GY-271, WS2812, buzzer.
- **Qwiic is a 3.3 V standard** — its connector gets 3.3 V, not 5 V (exception to rule 3.1.1, by standard).

## 6. Head link — 4-wire harness

| Pin | Signal | Direction |
|---|---|---|
| 1 | 5 V logic | base → head |
| 2 | GND | — |
| 3 | TX (base) → RX (head) | base → head, 3.3 V |
| 4 | RX (base) ← TX (head) | head → base, 3.3 V |

Connector: JST-XH 4-pin on the base; the head side depends on the chosen camera board (usually an adapter lead). Pin order final in the LLD. (A 6-wire harness with servo power to the head was considered and dropped with the head servos, owner 2026-10-14.)

## 7. Camera head

An **existing ESP32-S3 camera board**, not designed by us. Selection criteria:
- ESP32-S3 with **PSRAM** (8 MB preferred), OV2640/OV3660 camera.
- **2 free GPIOs** for the UART (not used by camera or PSRAM) and a **5 V input** pin.
- **U.FL external antenna** preferred (metal chassis).
- USB-C for recovery; normal updates over WiFi (OTA).

Candidates: Seeed XIAO ESP32S3 Sense (U.FL, small), Freenove ESP32-S3-WROOM CAM. Not recommended: AI-Thinker ESP32-CAM (old ESP32, no USB, few pins).

Head firmware = the current ESP32 bridge (Plan C) with board pin profiles per camera board. Pan/tilt stays on the base, as today.

## 8. Base firmware (what the port needs)

Reuse the current body firmware's design: protocol, domain logic (safety, control, estimation, IR, settings), and tests. New:
- **Board layer for RP2350** (Arduino-Pico core): hardware PWM for motors/servos/buzzer, **PIO** for encoders, WS2812, IR and RC input, hardware I2C (no software I2C), ADC for the input voltage, the servo current and the 6 servo feedback pins; VBUS as a digital input (Rev 1).
- **FPU:** the IMU/PID float maths that cost ~2.4 ms per pass on the Mega become cheap.
- **Settings** in flash (EEPROM emulation with wear levelling) instead of AVR EEPROM.
- New behaviour: motors (not servos) blocked while USB VBUS is present; in the arm role the body link runs over USB CDC serial, and a STOP freezes the arm (servo rail stays on, each servo holds its last pulse — Rev 1, §12.1); GPIO 8-11 set up as motor C/D or servo 3-6 from the MOTOR/SERVO switch at boot (live change → outputs off until reboot); RC input mapped like the IR remote (an input source under the arbitrator).
- HELLO reports the board type and fitted options (encoders, compass, 4-channel).

Tank Logo, the phone page, the bench tool and the AI engine work unchanged through the head.

## 9. Validation

To be written when we get there (owner, 2026-10-14). It will cover bring-up order, power and current checks, connector and 5 V-tolerance checks, and a port of the current hardware sign-off (deadman, link loss, obstacle brake, soak).

## 10. Open questions

| # | Question | Notes |
|---|---|---|
| O1 | ~~Encoders for motors C/D?~~ | **Decided: RP2354B** - 4 encoders + expansion header (owner, 2026-10-14) |
| O2 | Exact parts: motor driver (TB6612 vs DRV8833-class), 5 V buck, LDO, reverse-polarity MOSFET, fuse | LLD |
| O3 | ~~One 5 V buck or a separate servo buck~~ | **Decided: separate servo buck** (6 servos, owner 2026-10-14) |
| O4 | Status LEDs: which signals, buffered how, placement (edge / side-view) | LLD + layout |
| O5 | Head connection: adapter lead to the chosen camera board | Depends on the chosen camera board |
| O6 | Certification: CE (RED), FCC, UKCA, RoHS; EN 71 / BIS toys if sold for children; Li-ion rules if packs ship | Research task; launch as a 14+ electronics board without cells defers the toy/battery rules (to confirm) |
| O7 | Track/chassis supplier (Conqueror/Devastator style) | design-notes §40 sourcing plan |
| O8 | Arm power (Rev 1, proposed, not decided): a firmware-controlled servo rail switch (STOP, soft start); start without a jump (read the feedback, command that pose, then switch the rail on; without feedback, a slow move from a remembered parked pose); a 6-8 A servo buck for arms (3x MG996R stall ~7.5 A) | Owner to decide; LLD |

## 11. Risks

- **Supply:** LCSC stock of RP2350-family chips has dropped sharply before — check before each order; reserve stock or use a Raspberry Pi approved reseller for production.
- **Power integrity:** motor and servo noise on a small board; needs careful layout (star ground, bulk caps, separate servo trace).
- **Compass accuracy** near motors — the socket is a convenience; reliable heading needs a remote (Qwiic) compass.
- **Cost:** the electronics target (~INR 1,000-1,500 / US$12-18 BOM) has to hold at small volumes; market research suggests a retail of INR 1,999-2,499 for the board (`docs/research/2026-10-14-base-board-market-india.md`).
- **Camera board** availability (off-the-shelf) must be checked per batch.

## 12. Revision 1 — arm-ready analog (owner, 2026-10-27, design-notes §159)

Why: the board is also an arm controller (§3.0), and cheap hobby servos (MG996R, SG90) report nothing. Reading each servo's own pot (a wire soldered to the pot's wiper, the "feedback mod") gives real joint angles: stall and blocked-joint checks, the sag under gravity, teach by hand, a start without a jump. The board should take that wire with no extra parts.

What changed:

| ADC | Before | Rev 1 |
|---|---|---|
| ADC0 | Input voltage | unchanged (in the servo jumper's **BATT** position the servo rail *is* the input, so this is also the rail voltage; on the 5 V / 6 V buck the rail is steady) |
| ADC1 | USB VBUS sense | **Servo rail current** (one shunt + amplifier): the only "something is straining" signal for servos without the feedback mod |
| ADC2-5 | Expansion analog, 3.3 V max | Expansion analog **and** servo 3-6 feedback (header pin 4), 0-9.9 V through a 1/3 divider |
| ADC6-7 | Reserved: motor current sense (future) | **Servo 1-2 feedback** (header pin 4), 0-9.9 V |
| — | VBUS on ADC1 | VBUS on **GPIO37** as a digital input (yes/no is all it needs); the GY-271 DRDY it replaces is not needed (the compass is polled) |

Rules and reasons:
1. **Native PWM, no on-board PCA9685** (owner): the RP2354B's PWM steps are ~15x finer, cost nothing and cannot hang an I2C bus; 6 outputs cover a 6-servo arm; more servos via a PCA9685 on Qwiic (no feedback on those).
2. **4-pin servo headers** (signal, V+, GND, feedback): a standard servo plug uses pins 1-3; the feedback wire plugs into pin 4. No ADS1115, no loose wires.
3. **One divider for every analog input (1/3, 0-9.9 V)**: the pot swings up to the servo rail (6 V on the buck, up to 8.4 V on BATT), above the ADC's 3.3 V; the same divider makes the expansion analog inputs 5 V-safe too, removing the one exception to rule 3.1.1. Cost: a 3.3 V sensor on the expansion header uses a third of the ADC range (~1 360 counts, still fine); input impedance ~30 kΩ.
4. **One role at a time, no switches** (owner: keep v1 simple): ADC2-5 reach both the expansion header and servo headers 3-6; plug into one or the other. An arm on a tank, and analog add-ons (a joystick) on an arm, are not v1 targets.
5. **Motor current sense dropped**: the tank already finds a stall from its encoders (Uno Lite LLD §19, Plan I); it was a future nice-to-have.
6. **The pot reading moves with the servo rail voltage**: calibrated per servo at the rail's voltage; on BATT the firmware scales by ADC0.

Considered and not taken: a TLA2528 8-channel ADC (~US$1) for the pots — keeps the expansion analog inputs free in arm mode and allows an arm on a tank; not needed while a board plays one role at a time (revisit if a mobile arm becomes a target).

### 12.1 The board as the arm platform's best tier (owner, 2026-10-27)
The arm platform is a separate project (`../arm-robot/docs/design-notes.md`): general-purpose software for cheap hobby servo arms; this board is its best tier, above the Uno R4 (recommended) and Uno R3 / Nano (minimum). What that asks of the board:
1. **USB is the arm's link to the laptop** (USB CDC serial): the arm sits on a desk by the laptop. Rule 3.1.4 (USB never drives motors) stays for motors; servos move with USB attached. The arm's protocol does not care how its bytes travel, so wireless later goes over the head link (an ESP32 as co-processor) or a Bluetooth serial module on the expansion UART1.
2. **STOP freezes the arm**: cutting servo power would drop it onto the table, a thing or a hand. The servo rail stays on and every servo holds its last pulse. A joint still straining after the freeze (servo current, feedback) and whether to go limp are for the arm's protocol design.
3. **A bulk capacitor on the servo rail** (1000-2200 µF near the servo headers): MG996R starts and stalls pull ~2.5 A each; the arm's reference builds use the same (LLD to size it).
4. A stationary arm runs from the 7-12 V adapter (§3.0); with 3x MG996R the servo buck's ~5 A is the limit — see O8.

