# Conqueror ESP32-S3 developer bridge (`firmware/esp32`, `env:bridge`)

> **DEVELOPER BRIDGE — NO AUTHENTICATION.** Anyone on the same network can drive the tank (Basic
> Drive page, WebSocket `/ws`). Use it only on your home network. Never ship it in
> release firmware. OTA is password-protected (MD5 challenge — fun, not real security).

Board: ELEGOO **ESP32S3-Camera-V1.0** (ESP32-S3-WROOM-1, 8 MB flash, 8 MB octal PSRAM, USB-C).
Design: `docs/lld/esp32-bridge-lld.md` Revision 2 · plan: `docs/superpowers/plans/2026-10-11-plan-c-esp32-bridge.md`.

What it does:
- **Basic Drive page** at `http://conqueror-bridge.local/` — joystick, STOP, speed, camera, telemetry.
  The ESP32 is the body-link master for the page (handshake, heartbeats); every drive message from
  the page becomes exactly one DRIVE frame, so a sleeping phone stops the tank (Uno deadman, 0.3 s).
- **Settings page** at `/settings` — WiFi networks, tank info, calibration (Plan M).
- **Camera** MJPEG at `http://conqueror-bridge.local:81/stream` (max 2 viewers).
- **OTA** over WiFi (ArduinoOTA, only while nobody drives) — the Uno stays USB-only.
- **Self-description**: `/info` (JSON), `/help` (people), `/source.tar.gz` (this exact source).

Needs **Plan B firmware on the body board** (Arduino Mega 2560 since 2026-10-13; "Uno" below means the body board; protocol version 2); with another version the page shows
"body offline — flash Plan B".

## Setup
1. `uv tool install --python 3.12 platformio && uv tool install --python 3.12 esptool` (Linux: udev rules + `dialout`, see `docs/dev-setup.md`).
2. `cp include/secrets.example.h include/secrets.h` — home WiFi, fallback-AP password (≥ 8 chars),
   OTA password (not `change-me`). `secrets.h` is git-ignored and never packed into the source archive.
3. Check the chip, then flash over USB-C (hold BOOT while plugging in if no port appears):
   ```bash
   python3 scripts/check_chip.py --port /dev/ttyACM0      # macOS: /dev/cu.usbmodem*
   pio run -e bridge -t upload --upload-port /dev/ttyACM0
   pio run -e bridge -t upload_source --upload-port /dev/ttyACM0
   pio device monitor -p /dev/ttyACM0 -b 115200            # boot log: version, IP, Uno HELLO
   ```
4. Shield switch **S1** in the "ESP32 connected" position (flip it only for Uno USB uploads).

## Use
| What | How |
|---|---|
| Drive from a phone | `http://conqueror-bridge.local/` (or the IP from the log); add to home screen for fullscreen |
| Camera only | `http://conqueror-bridge.local:81/stream` |
| Facts | `/info`, `/help`, `/source.tar.gz` |
| Update over WiFi | `CQ_OTA_PASSWORD=… scripts/ota.sh` (app + source; refused while someone drives) |
| Release | `git tag bridge-vX.Y.Z && scripts/make_release.sh bridge-vX.Y.Z` |
| Host tests | `pio test -e native` |
| Smoke checks | `cd ../../tools/smoke && uv run python bridge_echo.py --host conqueror-bridge.local no-tcp|ws|ws-owner` |

Rules worth knowing: one driver at a time (first `drive` owns it, 5 s handback); other phones get
`NOT_OWNER`; STOP works from everyone, always, and holds until the driver releases its stick; the IR remote beats everyone. Home WiFi
unreachable for 30 s → hotspot `conqueror-bridge` at `192.168.4.1`.

## Hardware checklist (owner present, tank on a stand, tracks off the ground, STOP within reach)
- [ ] Chip check: `check_chip.py` prints `OK - ESP32-S3, 8MB flash, 8MB PSRAM, MAC 48:ca:43:9e:74:b4`; a non-S3 board is refused.
- [ ] USB flash on Linux via `/dev/ttyACM0`; boot log shows version, commit, `PSRAM 8 MB`, `running from partition app0|app1`.
- [ ] Uno link: `uno: synced fw … proto 2 commit …` after a phone connects; unplugging the Uno cable gives `body offline`.
- [ ] WiFi: station IP + `conqueror-bridge.local`; wrong password → fallback AP after 30 s.
- [ ] Old bench port closed: `bridge_echo.py no-tcp` → `port 3333 closed` (design-notes §74).
- [ ] Phone drive: forward/turn/hold still keeps moving/release stops/STOP stops/speed slider scales.
- [ ] Deadman: phone locks its screen while driving → tracks stop within ~0.3 s; unlock → no movement until the stick is touched again.
- [ ] Ownership: second phone sees "Busy"; IR remote takes over.
- [ ] Camera: video on the page, pan/tilt stick returns home, third viewer gets 503.
- [ ] OTA: `scripts/ota.sh` while idle works; while a phone drives, espota times out.
- [ ] `/info` valid JSON with `uno.commit`, `source.matches_fw: true`; `/help` complete; `/source.tar.gz` has no `secrets.h`.
