# Old Uno firmware — reverse-engineered serial JSON protocol

Source: `~/tank-dump/uno-flash.hex` (Linux box) / `scratchpad/uno/uno.bin`. Disassembled with
`avr-objdump -D -m avr5 -b ihex` (PlatformIO `toolchain-atmelavr` 3.70300). Read-only analysis; nothing
was sent to any device.

## TL;DR

| Item | Value | Confidence |
|---|---|---|
| Serial | USART0 (D0/D1), **115200 8N1** (U2X=1, UBRR0=16) | high |
| Framing | one JSON object per line, terminated by `\n` or `\r`; max 127 chars (longer lines are discarded) | high |
| Command selector | key **`"N"`** (integer) | high |
| Drive | `{"N":4,"D1":<left>,"D2":<right>}` — signed track speeds, effective range −255..255 | high (left/right naming: medium) |
| STOP | `{"N":4,"D1":0,"D2":0}` | high |
| Deadman | drive auto-stops if no new N=4 for **≥ 501 ms** | high |
| Safest test move | `{"N":4,"D1":110,"D2":110}` once, then STOP within ~300 ms (it would self-stop at ~0.5 s anyway) | high |

## 1. Memory map facts

- `.data` copy loop at `0x288..0x29c`: flash `0x7BE6` → RAM `0x0100..0x01EF` (so RAM = flash − 0x7AE6).
- Key strings in RAM: `"D2"`=0x1D7, `"V"`=0x1DA, `"Y"`=0x1DC, `"P"`=0x1DE, `"R"`=0x1E0, `"D1"`=0x1E2,
  `"A"`=0x1E5, `"T"`=0x1E7, `"M"`=0x1E9, `"B"`=0x1EB, `"TN"`=0x1ED.
  **`"N"` is 0x1EE** — the linker merged it into the tail of `"TN"`, which is why no stand-alone "N" string exists.
- `Serial` (HardwareSerial) object at RAM 0x040C. `millis()` = `0x560` (reads 0x398..0x39B).
  `digitalWrite` = `0x322`, `pinMode` = `0x37E`, `analogWrite` = `0x3F6` (starts with pinMode OUTPUT).

## 2. Baud rate (evidence)

`setup` at `0x4d24..0x4d44` (inlined `HardwareSerial::begin`):
`*ucsra = 0x02` (U2X0), `*ubrrh = 0`, `*ubrrl = 0x10` → 16 MHz / (8 × 17) = 117 647 ≈ **115200 baud**;
`*ucsrc = 0x06` → 8N1. (The `sts 0x00C4` at 0x7E24 is inside Optiboot, not the app.)
Same block: `pinMode(3,OUTPUT); digitalWrite(3,HIGH)` at `0x4d14..0x4d20` → **STBY (D3) is held high permanently**.

## 3. Receive / parse path (in the big `loop()` function, ~0x5070..0x6540)

- Every **20 ms** (`0x5c1e`, millis − 0x387 ≥ 20) the loop drains `Serial.available()` (`0x8b8`) / `read()` (`0x874`).
- Chars are appended to a buffer at RAM 0x2D5 (index 0x355, limit 0x7F — `0x650c`); index ≥127 → buffer reset (`0x6522`).
- On `\n` (0x0A) or `\r` (0x0D) (`0x5c64..0x5c68`) the buffer is NUL-terminated and `deserializeJson` runs (`0x5f26`).
  Empty line is ignored. Parse error → ignored.
- On success: `0x036C` (IR-remote-active flag) is cleared (`0x5f5e`), then `N = doc["N"].as<int>()` (`0x5f62..0x5f7a`, key ptr r4:r5 = 0x1EE set at `0x5c38`; r14:r15 = 0x1E2 "D1").

## 4. Command table (switch at `0x5f7e..0x6012`)

| N | Handler | Params | Effect | Confidence |
|---|---|---|---|---|
| **4** | `0x5f9c` → `0x1b30` | `D1` int, `D2` int | `0x2B6 = D1`, `0x2B4 = D2`, `0x2B0 = millis()` (deadman timestamp), cancels any gyro turn (`0x2D4 = 0`). **Drive.** | high |
| 10 | `0x60fc` | `D1` bool/number | sets flag `0x2C9` (D1 truthy → 1). Changes drive-mode logic at `0x56d0` (unknown assist mode). Don't use. | low |
| 11 | `0x60d6` | `D1` int | stores into `0x371` (reported as `"T"` in telemetry; alters motor logic at `0x56c8`). Don't use. | low |
| 20 | `0x61f2` | `D1` angle (int/float), `D2` speed (optional, default 110) | **gyro turn-in-place by angle**: sets `0x2CC` target, `0x2D4 = 1`, `0x2CA` speed; loop at `0x5368` spins ±speed until yaw reaches target. | medium |
| 24 | `0x6014` → `0x40dc` | — | send one telemetry JSON line immediately | high |
| 26 | `0x633e` | `D1` (1..13 via jump table) | some 13-way selector (likely LED colour / sound). Not decoded. | low |
| 106 | `0x601a` | `D1` 0..180, `D2` 30..110 (optional) | camera servos: D1 → pan angle (clamped ≤180, negative ignored) at `0x10F`; if `D2` present (`containsKey` `0x365a`) and 30 ≤ D2 ≤ 110 → tilt `0x11F`; then servo update `0x1b1a`. | medium |

Any other N is ignored.

## 5. Drive semantics (`0x5304..0x5858`, runs every 10 ms)

1. **Deadman** (`0x530c..0x535a`): if IR mode flag `0x36C == 0` and (D1 ≠ 0 or D2 ≠ 0) and
   `millis() − 0x2B0 ≥ 0x1F5` (**501 ms**) → D1 = D2 = 0. So every N=4 move self-expires ~0.5 s after the last N=4.
2. Gyro heading-hold correction (`0x5520..0x5698`): when |D1−D2| < ~20 and both non-zero, a yaw PID term clamped to ±80 is
   computed; left = D1 − corr, right = D2 + corr, each clamped to −255..255. On a stand the yaw does not change, so corr ≈ 0.
3. **Safety stops**:
   - Obstacle: if ultrasonic distance (`0x143`, init 999) < **25 cm** and both tracks forward → stop (state 3) + tone (`0x56ec..0x5766`).
   - Tilt: |pitch| or |roll| > 35° (`0x420C0000` float at `0x5720/0x5742`) → stop.
4. Per side (`0x57c8..0x5858`): if |speed| < **80** → PWM 0 immediately (deadband). Otherwise target = min(|speed|,255),
   then ramp `0x1b60`: on direction change while running → PWM 0 first; from 0 it jumps straight to **max(target, 110)**;
   otherwise steps ±6 per 10 ms toward target with a floor of 110. ⇒ **lowest real PWM is 110/255 (~43 %)**; any |D| in 80..110 gives 110.
5. Outputs:
   - `digitalWrite(7, dirLeft>0)`, `analogWrite(6, pwm from D1)`
   - `digitalWrite(8, dirRight>0)`, `analogWrite(5, pwm from D2)`
   - Positive value → direction pin HIGH (treated as forward).

**Pin-pairing caveat (medium confidence on naming):** the firmware pairs **D1 → PWM D6 + dir D7** and **D2 → PWM D5 + dir D8**.
Our hardware facts say PWMA D5 pairs with AIN1 D7 and PWMB D6 with BIN1 D8. So with D1 ≠ D2 signs the directions may be
crossed. With **D1 = D2 (same value, same sign) this does not matter**, so the test uses equal values.

## 6. Telemetry (Uno → ESP32, informational)

`0x40dc` sends every ~100 ms (`0x6884`) and on N=24, one line:
`{"N":30,"V":<0x133>,"Y":<0x137>,"P":<0x13B>,"R":<0x13F>,"D1":…,"D2":…,"A":…,"T":<0x371>,"M":…,"B":<turn-active 0x2D4>,"TN":…}`
(V likely battery voltage, Y/P/R yaw/pitch/roll; other fields not decoded.) Seeing these lines is a good link check.

## 7. Recommended test (tank on stand, tracks off the ground, nothing within 25 cm in front of the ultrasonic sensor)

Each command is one line ending in `\n` (if the ESP32 forwards UDP verbatim, put the `\n` in the datagram).

1. STOP first: `{"N":4,"D1":0,"D2":0}\n`
2. Forward pulse: `{"N":4,"D1":110,"D2":110}\n` — both tracks forward at PWM 110 (the firmware minimum).
3. STOP within ~300 ms: `{"N":4,"D1":0,"D2":0}\n` (send it 2–3 times for UDP loss). Even if every STOP is lost,
   the deadman stops the motors ~0.5 s after the move.

Do not use N=20 (gyro turn: it refreshes its own deadman timestamp at `0x5d10` while turning and stops on reaching the
yaw target, which may never happen on a stand; any later N=4 cancels it), and avoid N=10/11/26 (not decoded).

## 8. Confidence summary

- High: baud 115200, line framing, `N` selector, N=4 drive with D1/D2, STOP, 501 ms deadman, 80 deadband / 110 floor, STBY always high.
- Medium: which track is "left" (pin pairing above), N=20 and N=106 semantics.
- Low: N=10, 11, 26 meaning; most telemetry fields.
