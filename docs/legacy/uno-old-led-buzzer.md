# Old Uno firmware — LED and buzzer sequences (reverse-engineered)

Source: `scratchpad/uno/uno.bin` / `d2.txt` (avr-objdump of `~/tank-dump/uno-flash.hex`). Read-only analysis; nothing was
sent to any device. Builds on `uno-old-protocol-analysis.md` (memory map: RAM = flash − 0x7AE6 for `.data`; PROGMEM tables
are read with `lpm` at their flash address).

## TL;DR

- **Buzzer**: Arduino `tone()` on **A0 (pin 14)**, Timer2 (`TIMER2_COMPA` ISR at `0x43F0`, `noTone` = `0x7A4`).
  All sounds go through one blocking player **`playMelody(const Note* PROGMEM, uint8 count)` at `0x23EA`**.
  `Note = {uint16 freqHz, uint16 durMs}` (4 bytes). Per note: `tone(14, f, d); delay(d)`; `f == 0` → `noTone(14); delay(d)`
  (a rest). `noTone` at the end. No extra gap between notes — the gaps are explicit rest entries.
  The player **blocks the whole loop** (LED, serial, drive) for the melody's total length.
- **13 melodies** in PROGMEM `0x097..0x1DA`, selectable with **`{"N":26,"D1":1..13}`**. Melody 1 = boot jingle,
  melody 5 = obstacle/tilt alarm chirp. Only 3 call sites exist: boot, obstacle/tilt alarm, N=26.
- **LED**: FastLED, WS2812 ×1 on D4 (unrolled clockless bit-bang at `~0x1E00..0x2100`, `out 0x0B`). `CRGB leds[1]` at RAM
  `0x364` (r,g,b = `0x364/0x365/0x366`), `FastLED.setBrightness` byte at `0x4B5`, `FastLED.show()` = `0x578`.
  Max brightness used anywhere is **50/255**. LED is refreshed every **30 ms** (`0x6544..0x6574`).
- **No low-battery sound** — battery is shown only by LED colour (thresholds 7.2/7.4 V and 6.7/6.9 V with hysteresis).
- **N=11 "T"** = safety-override toggle (T≠0 disables the obstacle + tilt stop and its alarm). Also toggled by the
  **mode button (D2)** and **IR key code 0x42**; each toggle blinks the LED 3× (blue = override ON, green = OFF).
- **N=10** `D1=1` → flag `0x2C9`: forces LED "mode 2" palette (purple / pink). No other effect found.

## 1. Sounds

Durations exact (ms); rests written as `—`. Note names are nearest equal-tempered notes (cents off in brackets).
Evidence: table address = PROGMEM start, N=26 dispatch `0x6394..0x650A` (jump table words at `0x63AA`).

| # (N=26 D1) | Table | Notes (Hz/ms) | Nearest notes | Total | Trigger(s) | Character |
|---|---|---|---|---|---|---|
| **1** | 0x1A3 (14) | 200/150, —/80, 500/120, —/60, 1000/100, —/40, 1500/60, 2000/60, 2500/60, 3000/60, —/60, 1500/60, —/40, 2000/150 | G3+35, B4+21, B5+21, F#6+23, B6+21, D#7+8, F#7+23, F#6, B6 | 1100 | **Boot** (end of `setup`, `0x5102..0x5108`) and N=26 D1=1 | "power-up" rising sweep + two-note tag |
| 2 | 0x17F (9) | 800/50, —/25, 1000/50, —/25, 1400/50, —/25, 1800/80, —/20, 2200/120 | G5+35, B5+21, F6+4, A6+39, C#7−14 | 445 | N=26 only | rising 5-step arpeggio ("ready/success") |
| 3 | 0x15B (9) | 1200/60, —/30, 1600/60, —/30, 2000/60, —/50, 2400/80, —/30, 2400/150 | D6+37, G6+35, B6+21, D7+37, D7+37 | 550 | N=26 only | rising 4 + repeated top ("fanfare") |
| 4 | 0x143 (6) | 1500/80, 1200/80, 800/100, —/50, 600/80, 400/200 | F#6+23, D6+37, G5+35, D5+37, G4+35 | 590 | N=26 only | falling ("sad / power-down") |
| **5** | 0x12F (5) | 2500/40, —/20, 3000/40, —/20, 2500/40 | D#7+8, F#7+23, D#7+8 | 160 | **Obstacle stop / tilt stop alarm** (`0x5760..0x5766`), repeats every loop pass while the condition holds; N=26 D1=5 | short high triple chirp |
| 6 | 0x127 (2) | 1800/40, 1400/60 | A6+39, F6+4 | 100 | N=26 only | two-note falling "blip" (ack/click) |
| 7 | 0x113 (5) | 800/150, —/80, 600/150, —/80, 400/250 | G5+35, D5+37, G4+35 | 710 | N=26 only | slow descending 3 tones ("error / fail") |
| 8 | 0x0F7 (7) | 1500/40, —/20, 2000/40, —/20, 2500/40, —/15, 2800/80 | F#6+23, B6+21, D#7+8, F7+4 | 255 | N=26 only | fast rising 4 ("confirm / found") |
| 9 | 0x0DF (6) | 800/60, 1200/60, 1600/80, —/30, 1200/60, 2000/100 | G5+35, D6+37, G6+35, D6+37, B6+21 | 390 | N=26 only | up-down-up ("happy / question") |
| 10 | 0x0D3 (3) | 1500/30, —/20, 2200/50 | F#6+23, C#7−14 | 100 | N=26 only | quick up-chirp ("tick / OK") |
| 11 | 0x0C3 (4) | 300/60, 250/60, 300/60, 250/80 | D4+37, B3+21, D4+37, B3+21 | 260 | N=26 only | low warble ("grumble / denied") |
| 12 | 0x0A7 (7) | 600/50, 1000/50, 1500/50, 2000/50, 1500/50, 1000/50, 600/50 | D5+37, B5+21, F#6+23, B6+21, F#6+23, B5+21, D5+37 | 350 | N=26 only | up-and-down sweep ("scan / siren") |
| 13 | 0x097 (4) | 1500/80, 1000/80, 600/80, 300/150 | F#6+23, B5+21, D5+37, D4+37 | 390 | N=26 only | falling 4 ("goodbye / shutdown") |

The descriptive names in the last column are my guesses from the contour; the firmware has no names (no strings).

### Alarm trigger detail (melody 5)

Drive loop (every 10 ms), `0x56C8..0x5766`, only when **T (`0x371`) == 0**:
- **Obstacle**: distance `0x143` < **25 cm** and **both** commanded track speeds > 0 (forward), or
- **Tilt**: |pitch| (`0x13B`) > **35°** or |roll| (`0x13F`) > **35°** (float `0x420C0000`, `__gtsf2` at `0x7698`).

→ motion state `0x35A = 3`, speeds forced to 0 for that pass, `playMelody(0x12F, 5)` (160 ms, blocking). Because it re-runs
each pass, the sound is a **continuous chirp** while tilted, or while forward commands keep arriving / until the 501 ms deadman
clears an obstacle-blocked command. Confidence: high (code), medium (exact "repeats" feel).

### Boot (setup `0x5020..0x5108`)
`sbi DDRD,4`; FastLED init (1 LED at `0x364`, min refresh interval 2500 µs); **LED = blue (0,0,255), brightness 50, show**;
pinMode(2, INPUT_PULLUP) (mode button); pinMode(A0, OUTPUT) + LOW; T = 0; D1 = D2 = 0; **melody 1** (1.1 s).

## 2. LED patterns

LED state machine at `0x6528..0x6BDE`, runs every **30 ms**, priority top to bottom:

| Prio | Condition | Colour CRGB(r,g,b) | Pattern / brightness (of 255) | Evidence | Confidence |
|---|---|---|---|---|---|
| 0 | **Boot** | (0,0,255) blue | solid, 50 (until first LED tick) | `0x506E..0x5082` | high |
| 1 | **T toggled** (button D2, IR key 0x42) — 3 blinks | T→1: (0,0,255) **blue**; T→0: (0,128,0) **green** | 6 phases of **80 ms**: on (50) / off, i.e. 3 × (80 on + 80 off) ≈ 480 ms | setter `0x2902` (count 6 at `0x36B`, colour `0x361..0x363`, start `0x367`), player `0x657E..0x662C` | high |
| 2 | **"Impact / tipped" latch** `0x147` set (raw accel |AX| or |AY| ≥ 15001 ≈ 0.92 g, `0x5AF0..0x5B14`) | (255,255,255) **white** | 80 ms white flashes re-armed every drive tick (`0x5772..0x5798`, flag `0x360`, ts `0x35C`) → near-constant white flicker. The latch is **never cleared** in code → lasts until reset (probably a bug) | `0x65C2..0x6606`, `0x69F8` | medium |
| 3 | **Alarm** state 3 (obstacle / tilt stop) | (255,0,0) **red** | blink **100 ms on / 100 ms off** (5 Hz), on = 50 | `0x6A04..0x6A34` (`millis/100 & 1`) | high |
| 4 | **Sharp turn / spin** state 2 (|D1−D2| ≥ 101) | (255,165,0) **orange** | blink **150 ms on / 150 ms off**, on = 50 | `0x6A36..0x6A64`, state set `0x5E44..0x5E76` | high |
| 5 | Battery **critical** (`0x358`=2) | (255,0,0) **red** | idle: breathe; driving: speed-scaled (see below) | `0x6AB6..0x6ABE` → `0x6B5A` | high |
| 6 | Battery **low** (`0x358`=1) | mode≠2: (255,140,0) **dark orange**; mode 2: (255,50,80) **pink** | idle: breathe; driving: speed-scaled | `0x6AC8..0x6ADA` | high |
| 7 | Battery OK, mode 0 (T=0, N=10 off) | (0,128,0) **green** | idle: breathe; driving: speed-scaled | `0x6B0A..0x6B16`, `0x6B68` | high |
| 7 | Battery OK, mode 1 (T≠0 = safety override) | (0,0,255) **blue** | same | `0x6B64` | high |
| 7 | Battery OK, mode 2 (N=10 D1=1) | (128,0,255) **purple** | same | `0x6B18..0x6B1E` | high |

"Mode" = `0x356` = T (N=11 value / button / IR toggle) unless the N=10 flag is set, which forces mode 2 (`0x56D0..0x56E2`).

Brightness for priorities 5–7:
- **Driving** (state 1: moving, |D1−D2| < 101): brightness = `10 + avgSpeed·40/255`, avgSpeed = (|D1|+|D2|)/2 (`0x359`,
  `0x5E82..0x5EAA`, `0x6B26..0x6B48`) → 10..50 with speed.
- **Idle** (state 0): **breathing**, triangle wave (`triwave8`) over the period, brightness = `10 + tri·40/255` (10..50).
  Period **2000 ms**, or **1000 ms** when V < 6.7 V (`0x6B6C..0x6BDA`).

## 3. Battery thresholds (V = `0x107`, float volts, copied from telemetry `"V"` `0x133`)

Hysteresis state `0x358` (`0x6A66..0x6B08`, constants `0x40E66666`=7.2, `0x40ECCCCD`=7.4, `0x40D66666`=6.7, `0x40DCCCCD`=6.9):

| From | To | Condition |
|---|---|---|
| OK (0) | LOW (1) | V < **7.2 V** |
| LOW (1) | OK (0) | V > **7.4 V** |
| LOW (1) | CRITICAL (2) | V < **6.7 V** |
| CRITICAL (2) | LOW (1) | V > **6.9 V** |

Plus: idle breathing speeds up to 1 s period below 6.7 V. No sound and no motor cut-off on low battery. Confidence: high.

## 4. Commands related to sound / LED / modes

| Command | Meaning | Evidence | Confidence |
|---|---|---|---|
| `{"N":26,"D1":k}` k = 1..13 | play melody k (table above), blocking. Other k ignored. | `0x633E..0x650A` | high |
| `{"N":11,"D1":t}` | set T (`0x371`): t ≠ 0 → **safety override** (obstacle + tilt stop and alarm skipped, `0x56E6`), LED mode 1 (blue). Reported as telemetry `"T"`. No blink when set by N=11. | `0x60D6..0x60F6`, `0x56C8..0x56EA` | high (effect), medium (intent) |
| Mode button D2 (press) / IR key 0x42 | toggle T (0↔1) + 3-blink (blue = ON, green = OFF) | `0x682E..0x6858`, `0x6EB0..0x6ECE` | high |
| `{"N":10,"D1":1}` | flag `0x2C9` = 1 → LED mode 2 (purple; low battery pink); `D1` other → 0. Only use found is the LED mode. | `0x60FC..0x61AC`, `0x56D0` | medium |

## 5. Suggestions for the new catalogue (not decisions)

Reusable as-is: boot jingle (#1), alarm chirp (#5), 3×80 ms confirm blink, red 5 Hz blink for safety stop, orange 3.3 Hz
blink for spin, speed-scaled brightness while driving, 2 s breathe idle, battery colour with hysteresis 7.2/7.4 and 6.7/6.9 V.
Do not copy: the blocking player (use a non-blocking sequencer), the never-cleared white "impact" latch, and the T override
that disables safety stops.
