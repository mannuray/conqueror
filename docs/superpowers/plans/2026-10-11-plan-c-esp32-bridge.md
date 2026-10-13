# Plan C — ESP32-S3 WiFi Bridge + Basic Drive Page Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Developer firmware for the ELEGOO **ESP32S3-Camera-V1.0** board that (a) joins the home WiFi (AP fallback), (b) passes the bench tool's TCP 3333 byte stream to the Uno frame-aligned (Plan D unchanged), (c) acts itself as the body-link master for a phone **Basic Drive page** served at `/` over a WebSocket `/ws`, (d) streams the camera (MJPEG :81, 2 viewers), (e) updates over WiFi (ArduinoOTA, password) and (f) describes itself (`/info`, `/help`, `/source.tar.gz`).

**Architecture:** One PlatformIO project `firmware/esp32/`. A **pure C++ core** (`src/core/`, no `Arduino.h`, host-tested in `env:native` with Unity): `FrameParser` (same resync contract as Plan A's Python parser, proven on `protocol/vectors/`), `FrameTx` (whole-chunk TX queue — frames from different sources never interleave), `BodyLink` (master: HELLO_REQ → HELLO → HELLO_ACK, HEARTBEAT 100 ms, STOP resend until ACK, telemetry snapshot), `LinkOwner` (Idle / WebHandshake / WebReady / WebDriving / TcpPassthrough arbiter), `FrameAligner` (frame boundaries in passthrough streams), `WsCodec` / `WsJson` / `WsClients` (JSON ⇄ frames). A thin **Arduino/IDF shell** (`src/net/`): one `link_task` owns UART, core objects and the TCP client; `esp_http_server` on port 80 (`/`, `/ws`, `/info`, `/help`, `/source.tar.gz`) and port 81 (`/stream`); `WifiManager`, mDNS, `OtaService`, `CameraService`. Build scripts stamp version + git commit, gzip the web assets into a header, pack a `git archive` into the `source` flash partition and refuse to flash anything that is not an ESP32-S3.

**Tech Stack:** PlatformIO (installed with `uv tool`), **pioarduino** platform release **55.03.311** (Arduino-ESP32 **3.3.11**, ESP-IDF **5.5.5**), C++17 (`gnu++17`), `esp_http_server` (WebSocket support), esp32-camera (bundled with the core), ESPmDNS, ArduinoOTA; Unity for host tests; one HTML file with inline CSS + plain JS (no framework, no npm, no build step); Python 3.12 + uv for build scripts and the smoke script (Plan A's `conqueror_protocol`).

**Spec:** `docs/lld/esp32-bridge-lld.md` **Revision 2 (2026-10-12)** — authoritative where it differs from the Revision 1 text above it — plus `docs/design-notes.md` §22 (board is an ESP32-S3), §24 (pin map, flash/PSRAM, USB), §27 (self-describing firmware, HELLO `commit`), §28 (Basic Drive page) and the 2026-10-12 line accepting **all nine R2.14 confirmations**. Also `docs/lld/esp32-firmware-lld.md` §10.1 (toolchain), `docs/design/hub-ux.md` (Phosphor theme), `docs/implementation-roadmap.md` phase 3, `docs/standards/coding-standards.md` §1/§3.

**Roadmap position:** Phase 3, alongside Plan D (bench tool). Depends on **Plan A** (merged: `cq_protocol.h`, `protocol/vectors/`, `conqueror_protocol`; Task 9 adds one protogen output). **Plan B must be flashed on the Uno first** — the bridge speaks **protocol version 2** (HELLO carries `commit: u32`) and refuses an Uno that reports another version (page shows `body_offline`). Plan D talks to the Uno through this bridge unchanged.

## Global Constraints

- **Platform:** commands are shown for macOS; on **Linux** use the equivalents in `docs/dev-setup.md` (uv install, serial port names `/dev/ttyACM0`/`/dev/ttyUSB0` + `dialout` group, PlatformIO udev rules, `avahi-browse` instead of `dns-sd`, hold-to-drive needs X11). The ESP32-S3 board enumerates as `303a:1001` → **`/dev/ttyACM0`** on Linux (confirmed, §24) and `/dev/cu.usbmodem*` on macOS (§23: some USB-C cables are not detected on macOS — use Linux or another cable).
- **Board (§22/§24):** ELEGOO ESP32S3-Camera-V1.0, module ESP32-S3-WROOM-1, chip ESP32-S3 (QFN56) rev v0.2, **8 MB flash**, **8 MB embedded octal PSRAM** (`qio_opi`), native USB-Serial/JTAG (GPIO19/20) with `ARDUINO_USB_CDC_ON_BOOT=1`. No status LED → status on the USB serial log, `/info` and the bench tool. Every USB flash runs `scripts/check_chip.py` first and refuses unless esptool reports an ESP32-S3 with 8 MB flash and 8 MB embedded PSRAM.
- **Uno link:** `Serial1`, **RX GPIO3, TX GPIO40, 250000 8N1**, RX buffer 2 KB, TX buffer 1 KB. GPIO3 is a strapping pin — input only after boot, no pull-down. GPIO 26–37 (flash/PSRAM) are never touched.
- **Camera:** ESP32-S3-EYE map (PWDN −1, RESET −1, XCLK 15, SDA 4, SCL 5, D7..D0 = 16, 17, 18, 12, 10, 8, 9, 11, VSYNC 6, HREF 7, PCLK 13), XCLK 20 MHz, JPEG VGA, 2 frame buffers in PSRAM; MJPEG on **port 81 `/stream`, max 2 viewers**; sensor auto-detected (OV3660 expected) and logged.
- **Partitions (8 MB, `partitions_cq8m.csv`):** nvs 0x9000/0x5000 · otadata 0xE000/0x2000 · app0 0x10000/0x300000 · app1 0x310000/0x300000 · source (data/spiffs) 0x610000/0x1E0000 · coredump 0x7F0000/0x10000.
- **Body link (Plan A/B contract):** frames `0xAA | len | type | seq | payload | crc8(poly 0x07)` built and decoded **only** with the generated `cq_protocol.h`, included from `firmware/uno/src/generated/` through `-I ../uno/src/generated` (one file, never copied, never edited). As master the ESP32 sends HEARTBEAT every **100 ms**, HELLO_REQ on start and every **1 s** until a HELLO with `proto == kProtocolVersion` (2) arrives, then HELLO_ACK + OWNER_HINT. **No ENVELOPE from the bridge** (R2.14 confirmation 5 — the brief's "HELLO/ENVELOPE/HELLO_ACK" is the bench's optional path, not the bridge's). STOP is resent every **50 ms** up to **10** times until the Uno's ACK. Uno WIFI_RESET_REQ / ADMIN_RESET_REQ are answered with ESP_ACK `UNAVAILABLE` (the bridge has no WiFi/admin store) and logged.
- **The ESP32 never drives on anyone's behalf:** it originates only HEARTBEAT, HELLO_REQ, HELLO_ACK, ESP_ACK, STOP, OWNER_HINT and CAM_HOME; every DRIVE / CAM_JOY is sent 1:1 for a received WS message. A silent page ⇒ no DRIVE ⇒ Uno deadman (300 ms). The page therefore **re-sends `drive` every 50 ms from a timer while the stick is held**, even when the stick does not move.
- **Link owner (R2.7, accepted):** bench owns the link from TCP connect; a phone owns driving from its first `drive` until `release_control`, **5 s** without `drive`, or its socket closes; other phones → `nack NOT_OWNER`; phone motion while the bench holds the link → `nack BUSY`; a bench arriving while a phone drives, or a second bench, is **accepted and closed at once**; bench silent **1 s** → dropped; WS client silent **3 s** → reaped; **STOP from anyone in every state** (injected in passthrough with `seq = lastSeq + 128`, its ACK swallowed); IR on the Uno beats all.
- **TCP 3333:** `TCP_NODELAY`, one client, frame-aligned both ways (partial frame flushed raw after **50 ms**, discarded on close), bad-CRC frames forwarded unchanged (passthrough stays transparent).
- **WebSocket `/ws`:** JSON text frames exactly as LLD R2.9 (spec 01 §4–6 names): in `hello`, `drive{v,w}`, `stop`, `cam_joy{x,y,active}`, `cam_home`, `release_control`, `ping{t0}`; out `welcome`, `tf` (10 Hz), `ts` (1 Hz), `ev`, `ack`, `nack`, `pong`. Max **4** WS clients, rate limit **25 msg/s** per client. JSON is parsed and written by our own fixed-buffer code (`WsCodec`, `WsJson`) — no JSON library, no heap.
- **WiFi:** station from git-ignored `include/secrets.h`; **30 s** without a connection → WPA2 fallback AP `conqueror-bridge` (password from `secrets.h`, ≥ 8 chars) at `192.168.4.1`; in AP mode retry the home network every **60 s** only while no WS/TCP/stream client is connected. mDNS host **`conqueror-bridge.local`**; services **`_cqbridge._tcp` port 3333** (Plan D's `discover` looks for exactly this; `_conqueror-bridge` would exceed the 15-character DNS-SD limit), `_http._tcp` port 80; ArduinoOTA adds `_arduino._tcp` port 3232.
- **OTA:** ArduinoOTA, port 3232, password `CQ_OTA_PASSWORD` from `secrets.h` (build fails when missing or `change-me`); `ArduinoOTA.handle()` is polled **only while LinkOwner is Idle, WebHandshake or WebReady** (nobody driving, no bench). Uno firmware stays **USB only**.
- **Memory / tasks:** allocation at startup only (fixed pools for WS out messages, no `String` in hot paths); `link_task` (core 1, 2 ms tick) is the only owner of the UART, the core objects and the TCP client; httpd handlers talk to it through one FreeRTOS queue; free internal heap ≥ **120 KB** in steady state (reported in `/info`).
- **Read-only self-description:** `/info`, `/help`, `/source.tar.gz` never control anything and never contain secrets; `secrets.h` is git-ignored and `pack_source.py` fails the build if a secret value appears in the archive.
- **Security:** **DEVELOPER BRIDGE — NO AUTHENTICATION.** Anyone on the same network can drive the tank (WS and TCP). Home network only, never in release firmware; the boot log, `/help` and the README say so.
- **Host tests:** `cd firmware/esp32 && pio test -e native` (Unity). Core code compiles with `-Wall -Wextra -Werror` on the host and on the ESP32.
- **Commits:** Conventional Commits, one per task, each ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Generated files are never edited by hand (`firmware/uno/src/generated/`, `firmware/esp32/src/generated/`, `protocol/vectors/`, …) — change `protocol/defs/*.yaml` or the generator and run `cd protocol && uv run protogen write`.
- **Hardware steps** (marked **[HW]**) are run with the owner present, **tank on a stand, tracks off the ground**, a STOP within reach (page STOP, bench Space, IR remote).

## Review Focus

1. **Phone goes to sleep / tab hidden while driving** → no more `drive` messages; the Uno deadman stops the tank within 300 ms; the bridge never repeats a DRIVE (Task 6 test `test_bridge_never_invents_drive`; Task 10 HW check "phone sleep").
2. **Holding the joystick still** → the page keeps sending `drive` every 50 ms so the deadman does not stop a tank that the driver is holding (Task 7 page code `startDriveTimer`; Task 10 HW check).
3. **Bench connects while a phone drives / second bench** → accepted and closed at once, logged; the phone keeps driving (Task 3 tests `test_bench_refused_while_phone_drives`, `test_second_bench_refused`).
4. **Phone STOP while the bench holds the link** → STOP frame injected between two whole bench frames with `seq = lastSeq + 128`, resent until the ACK, ACK not forwarded to the bench (Task 2 `test_stop_resend_until_ack`, Task 4 `test_ack_swallowed_once`, `test_injected_frame_waits_for_partial`).
5. **Master change** (web → bench → web) → heartbeats stop at a frame boundary; when the web side takes over again it always starts with HELLO_REQ and only sends HELLO_ACK for `proto == 2` (Task 2 `test_handshake_rejects_wrong_proto`, Task 3 `test_bench_close_with_ws_clients_rehandshakes`).
6. **IR remote takes over** → web driver dropped without STOP (the Uno already stopped), web motion nacked `NOT_OWNER` until the Uno reports another owner (Task 3 `test_ir_beats_phone`).
7. **Interleaving** → TCP bytes and ESP32-originated frames never mix mid-frame on the UART; bad-CRC bench frames still pass (Task 2 `test_urgent_waits_for_chunk_boundary`, Task 4 `test_bad_crc_frame_forwarded`).
8. **Secrets** → `secrets.h` never tracked, never in `/source.tar.gz`, never logged (Task 9 `pack_source.py` checks, Task 1 `.gitignore`).

---

## File Structure

```
firmware/esp32/
├── platformio.ini                  envs bridge, bridge_release, bridge_ota, native (Task 1)
├── partitions_cq8m.csv             8 MB layout (Task 1)
├── README.md                       usage, security warning, hardware checklist (Task 10)
├── include/
│   ├── board_config.h              pins, ports, timings, sizes (Task 1)
│   ├── secrets.example.h           committed template (Task 1)
│   └── secrets.h                   git-ignored, local only
├── scripts/
│   ├── stamp_version.py            pre: build/version.h (version, git commit, dirty, date) (Task 1)
│   ├── check_chip.py               esptool flash-id gate, also pre-upload hook (Task 1)
│   ├── protogen_check.py           pre: protogen check (Task 1)
│   ├── native_paths.py             native env: CQ_VECTORS_PATH (Task 2)
│   ├── pack_web.py                 pre: gzip web assets + icon → build/web_assets.h (Task 7, help in Task 9)
│   ├── pack_source.py              pre: git archive → build/source.bin + secret checks (Task 9)
│   ├── upload_source.py            custom targets upload_source / upload_source_ota (Task 9)
│   └── ota.sh                      app + source over WiFi (Task 9)
├── src/
│   ├── main.cpp                    setup()/loop() composition root (Task 1, grows)
│   ├── core/                       pure C++ — no Arduino.h, host-tested
│   │   ├── frame_parser.h/.cpp     resyncing parser (Task 2)
│   │   ├── frame_tx.h/.cpp         whole-chunk TX queue + urgent slot (Task 2)
│   │   ├── body_link.h/.cpp        master: handshake, heartbeat, STOP resend, snapshot, events (Task 2)
│   │   ├── link_owner.h/.cpp       who is master / driver (Task 3)
│   │   ├── frame_aligner.h         passthrough frame boundaries, 50 ms flush, ACK swallow (Task 4)
│   │   ├── ws_codec.h/.cpp         JSON in → WsCommand (Task 6)
│   │   ├── ws_json.h/.cpp          snapshot/events → JSON out (Task 6)
│   │   └── ws_clients.h/.cpp       client table: names, rate limit, silence reap (Task 6)
│   ├── net/                        Arduino / ESP-IDF shell
│   │   ├── log.h/.cpp              USB serial status log (Task 1)
│   │   ├── uart_port.h/.cpp        Serial1 wrapper (Task 2)
│   │   ├── link_task.h/.cpp        owns core + UART + TCP; command queue (Task 2, grows 3/4/6/9)
│   │   ├── tcp_passthrough.h/.cpp  port 3333 (Task 4)
│   │   ├── wifi_manager.h/.cpp     station / fallback AP (Task 5)
│   │   ├── ota_service.h/.cpp      ArduinoOTA (Task 5)
│   │   ├── http_server.h/.cpp      port 80: /, /ws, assets, /info, /help, /source.tar.gz (Tasks 6, 7, 9)
│   │   ├── camera_service.h/.cpp   esp32-camera, S3-EYE pins (Task 8)
│   │   ├── stream_server.h/.cpp    port 81 /stream MJPEG (Task 8)
│   │   ├── info_service.h/.cpp     /info JSON (Task 9)
│   │   └── source_archive.h/.cpp   "source" partition reader (Task 9)
│   └── generated/help_protocol.html   protogen output — DO NOT EDIT (Task 9)
├── web/
│   ├── basic_drive.html            the Basic Drive page (Task 7)
│   ├── manifest.webmanifest        add-to-home-screen (Task 7)
│   ├── help.html                   /help template (Task 9)
│   └── help_logo.html              Tank Logo quick reference (hand-written, Task 9)
└── test/                           Unity, env:native
    ├── test_frame_parser/  test_frame_tx/  test_body_link/  test_link_owner/
    └── test_frame_aligner/ test_ws_codec/  test_ws_json/    test_ws_clients/
protocol/generator/protogen/emit_help.py      new protogen output (Task 9, Plan A addition)
protocol/generator/tests/test_emit_help.py    (Task 9)
tools/smoke/bridge_echo.py                    TCP smoke script (Task 10; uv project shared with Plan B)
.gitignore                                    + firmware/esp32/include/secrets.h (Task 1)
.gitattributes                                export-ignore PNGs for the source archive (Task 9)
```

> **Rewrite in progress:** Tasks 1–10 are being written; this marker is removed when the plan is complete.

---

### Task 1: PlatformIO project for the ESP32-S3, partitions, secrets, version stamp, chip check, boot log

**Files:**
- Create: `firmware/esp32/platformio.ini`, `firmware/esp32/partitions_cq8m.csv`, `firmware/esp32/include/board_config.h`, `firmware/esp32/include/secrets.example.h`, `firmware/esp32/include/secrets_guard.h`, `firmware/esp32/scripts/stamp_version.py`, `firmware/esp32/scripts/protogen_check.py`, `firmware/esp32/scripts/check_chip.py`, `firmware/esp32/src/net/log.h`, `firmware/esp32/src/net/log.cpp`, `firmware/esp32/src/main.cpp`
- Modify: `.gitignore` (add `firmware/esp32/include/secrets.h`; `build/` and `.pio/` are already ignored)

**Interfaces:**
- Consumes: `firmware/uno/src/generated/cq_protocol.h` (Plan A) via `-I ../uno/src/generated`.
- Produces: envs `bridge`, `bridge_release` (refuses a dirty tree), `bridge_ota` (espota, Task 5/9), `native` (host tests of `src/core/`); `cq::board::*` constants (pins, ports, timings, camera map); `build/version.h` macros `CQ_FW_NAME`, `CQ_FW_VERSION`, `CQ_GIT_COMMIT` (40 hex), `CQ_GIT_COMMIT8` (u32), `CQ_GIT_DIRTY`, `CQ_BUILD_DATE`, `CQ_REPO_URL`; `secrets.h` macros `CQ_WIFI_SSID`, `CQ_WIFI_PASSWORD`, `CQ_AP_PASSWORD`, `CQ_OTA_PASSWORD`; `cq::net::logBegin()`, `cq::net::logf(fmt, ...)`; `scripts/check_chip.py --port P | --self-test` (also a pre-upload action for every USB upload).

- [ ] **Step 1: Install PlatformIO and esptool**

Run: `command -v uv || brew install uv; uv tool install platformio && uv tool install esptool && pio --version && esptool version`
Expected: `PlatformIO Core, version 6.x.y` and an esptool version line (v4.8+ or v5.x). Linux: same commands; also install the PlatformIO udev rules and join `dialout` (`docs/dev-setup.md` §1).

- [ ] **Step 2: Create the project files**

`firmware/esp32/platformio.ini`:
```ini
; Conqueror ESP32-S3 firmware (ELEGOO ESP32S3-Camera-V1.0).
; env:bridge = developer WiFi bridge + Basic Drive page (NO AUTHENTICATION - home network only).
; env:native = host tests of src/core (Unity). env:gateway (full firmware) arrives in phase 4.

[platformio]
default_envs = bridge

[esp32]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
framework = arduino
board = esp32-s3-devkitc-1
board_build.flash_size = 8MB
board_upload.flash_size = 8MB
board_build.arduino.memory_type = qio_opi
board_build.partitions = partitions_cq8m.csv
monitor_speed = 115200
monitor_filters = esp32_exception_decoder
upload_speed = 921600
build_unflags = -std=gnu++11 -std=gnu++2b
build_flags =
    -std=gnu++17
    -I ../uno/src/generated
    -I build
    -D BOARD_HAS_PSRAM
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D CORE_DEBUG_LEVEL=1
; framework headers are not warning-clean, so -Werror is enforced on src/core in env:native
build_src_flags = -Wall -Wextra
extra_scripts =
    pre:scripts/protogen_check.py
    pre:scripts/stamp_version.py
    post:scripts/check_chip.py

[env:bridge]
extends = esp32

[env:bridge_release]
extends = esp32
build_flags = ${esp32.build_flags} -D CQ_RELEASE_BUILD

[env:bridge_ota]
extends = esp32
upload_protocol = espota
upload_port = conqueror-bridge.local
upload_flags =
    --port=3232
    --auth=${sysenv.CQ_OTA_PASSWORD}

[env:native]
platform = native
build_src_filter = +<core/>
test_build_src = yes
build_flags = -std=gnu++17 -I ../uno/src/generated -D CQ_HOST_TEST
build_src_flags = -Wall -Wextra -Werror
```

`firmware/esp32/partitions_cq8m.csv`:
```csv
# ESP32-S3 8 MB layout (bridge LLD R2.2). Changing it needs a USB flash.
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x9000,   0x5000,
otadata,  data, ota,      0xE000,   0x2000,
app0,     app,  ota_0,    0x10000,  0x300000,
app1,     app,  ota_1,    0x310000, 0x300000,
source,   data, spiffs,   0x610000, 0x1E0000,
coredump, data, coredump, 0x7F0000, 0x10000,
```

`firmware/esp32/include/board_config.h`:
```cpp
// ELEGOO ESP32S3-Camera-V1.0: pins, ports and sizes (bridge LLD R2.1, design-notes §24).
#pragma once

#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace board {

constexpr const char* kBoardName = "ESP32S3-Camera-V1.0";
constexpr const char* kExpectedMac = "48:ca:43:9e:74:b4";  // owner's board (warning only)

// Uno link: Serial1. GPIO3 is a strapping pin: input only after boot, no pull-down.
constexpr int kUnoRxPin = 3;    // <- Uno TX
constexpr int kUnoTxPin = 40;   // -> Uno RX
constexpr uint32_t kUnoBaud = 250000UL;
static_assert(kUnoBaud == proto::kBaud, "UART baud must match protocol/defs/meta.yaml");
constexpr int kUartRxBufSize = 2048;
constexpr int kUartTxBufSize = 1024;

constexpr uint32_t kLogBaud = 115200UL;  // USB-CDC log
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kStreamPort = 81;
constexpr uint16_t kTcpPort = 3333;
constexpr uint16_t kOtaPort = 3232;

constexpr const char* kHostname = "conqueror-bridge";  // mDNS conqueror-bridge.local
constexpr const char* kFallbackSsid = "conqueror-bridge";
constexpr uint32_t kStationTimeoutMs = 30000;
constexpr uint32_t kHomeRetryMs = 60000;
constexpr uint32_t kStatusPeriodMs = 10000;
constexpr uint32_t kLinkTickMs = 2;

constexpr uint8_t kMaxStreamViewers = 2;

// Camera: ESP32-S3-EYE map (design-notes §24). -1 = not connected.
constexpr int kCamPwdn = -1;
constexpr int kCamReset = -1;
constexpr int kCamXclk = 15;
constexpr int kCamSda = 4;
constexpr int kCamScl = 5;
constexpr int kCamD7 = 16;
constexpr int kCamD6 = 17;
constexpr int kCamD5 = 18;
constexpr int kCamD4 = 12;
constexpr int kCamD3 = 10;
constexpr int kCamD2 = 8;
constexpr int kCamD1 = 9;
constexpr int kCamD0 = 11;
constexpr int kCamVsync = 6;
constexpr int kCamHref = 7;
constexpr int kCamPclk = 13;
constexpr uint32_t kCamXclkHz = 20000000UL;

}  // namespace board
}  // namespace cq
```

`firmware/esp32/include/secrets.example.h`:
```cpp
// Template for include/secrets.h (git-ignored). Copy, then fill in:
//   cp include/secrets.example.h include/secrets.h
// Never commit secrets.h. pack_source.py fails the build if these values appear in the source archive.
#pragma once

#define CQ_WIFI_SSID "your-home-ssid"
#define CQ_WIFI_PASSWORD "your-home-password"
// Fallback hotspot "conqueror-bridge" (WPA2): at least 8 characters.
#define CQ_AP_PASSWORD "change-me-too"
// ArduinoOTA password (also export it as CQ_OTA_PASSWORD for `pio run -e bridge_ota -t upload`).
// The build refuses "change-me".
#define CQ_OTA_PASSWORD "change-me"
```

`firmware/esp32/include/secrets_guard.h`:
```cpp
// Compile-time checks on include/secrets.h (included by main.cpp only).
#pragma once

#if !__has_include("secrets.h")
#error "include/secrets.h missing: cp include/secrets.example.h include/secrets.h and fill it in"
#endif
#include "secrets.h"

#if !defined(CQ_WIFI_SSID) || !defined(CQ_WIFI_PASSWORD) || !defined(CQ_AP_PASSWORD) || \
    !defined(CQ_OTA_PASSWORD)
#error "secrets.h must define CQ_WIFI_SSID, CQ_WIFI_PASSWORD, CQ_AP_PASSWORD, CQ_OTA_PASSWORD"
#endif

namespace cq {
namespace secrets_guard {
constexpr bool streq(const char* a, const char* b) {
  return *a == *b && (*a == '\0' || streq(a + 1, b + 1));
}
static_assert(!streq(CQ_OTA_PASSWORD, "change-me"), "set CQ_OTA_PASSWORD in secrets.h");
static_assert(sizeof(CQ_OTA_PASSWORD) >= 9, "CQ_OTA_PASSWORD: at least 8 characters");
static_assert(sizeof(CQ_AP_PASSWORD) >= 9, "CQ_AP_PASSWORD: WPA2 needs at least 8 characters");
}  // namespace secrets_guard
}  // namespace cq
```

Append to the repository's `.gitignore`:
```text

# ESP32 bridge secrets (WiFi, AP and OTA passwords) - never commit
firmware/esp32/include/secrets.h
```

- [ ] **Step 3: Build scripts — version stamp, protocol freshness, chip check**

`firmware/esp32/scripts/stamp_version.py`:
```python
"""PlatformIO pre-script: write build/version.h (bridge LLD R2.3 step 1, design-notes §27).

CQ_FW_VERSION = git describe --tags --match 'bridge-v*' --always --dirty
CQ_GIT_COMMIT = full HEAD hash, CQ_GIT_COMMIT8 = first 8 hex digits as a number (0 = unknown).
env:bridge_release refuses a dirty tree. Plain Python + git: same on macOS and Linux.
"""

import datetime
import pathlib
import subprocess

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

project = pathlib.Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]  # noqa: F821
env_name = env.subst("$PIOENV")  # type: ignore[name-defined]  # noqa: F821


def git(*args: str) -> str:
    try:
        return subprocess.run(
            ["git", *args], cwd=project, capture_output=True, text=True, check=True
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


commit = git("rev-parse", "HEAD")
version = git("describe", "--tags", "--match", "bridge-v*", "--always", "--dirty") or "unknown"
dirty = 1 if git("status", "--porcelain", "--untracked-files=no") else 0
try:
    commit8 = int(commit[:8], 16)
except ValueError:
    commit8 = 0
built = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

if env_name.endswith("_release") and (dirty or not commit):
    print("stamp_version: release build refused - commit or stash your changes first")
    env.Exit(1)  # type: ignore[name-defined]  # noqa: F821

out = project / "build" / "version.h"
out.parent.mkdir(exist_ok=True)
out.write_text(
    "// generated by scripts/stamp_version.py - do not edit, git-ignored\n"
    "#pragma once\n"
    '#define CQ_FW_NAME "conqueror-bridge"\n'
    f'#define CQ_FW_VERSION "{version}"\n'
    f'#define CQ_GIT_COMMIT "{commit or "unknown"}"\n'
    f"#define CQ_GIT_COMMIT8 0x{commit8:08X}UL\n"
    f"#define CQ_GIT_DIRTY {dirty}\n"
    f'#define CQ_BUILD_DATE "{built}"\n'
    '#define CQ_REPO_URL "https://github.com/mannuray/conqueror"\n',
    encoding="utf-8",
)
print(f"stamp_version: {version} commit={commit[:8] or '?'} dirty={dirty}")
```

`firmware/esp32/scripts/protogen_check.py`:
```python
"""PlatformIO pre-script: fail the build when generated protocol files are stale (LLD R2.3 step 2)."""

import pathlib
import shutil
import subprocess

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

if env.subst("$PIOENV") != "native":  # type: ignore[name-defined]  # noqa: F821
    protocol = pathlib.Path(env.subst("$PROJECT_DIR")).parents[1] / "protocol"  # type: ignore[name-defined]  # noqa: F821
    if shutil.which("uv") is None:
        print("protogen_check: uv not found (see docs/dev-setup.md)")
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    result = subprocess.run(["uv", "run", "protogen", "check"], cwd=protocol, check=False)
    if result.returncode != 0:
        print("protogen_check: stale generated files - run: cd protocol && uv run protogen write")
        env.Exit(1)  # type: ignore[name-defined]  # noqa: F821
    print("protogen_check: generated files are fresh")
```

`firmware/esp32/scripts/check_chip.py`:
```python
"""Refuse to flash anything that is not the ESP32-S3 N8R8 board (bridge LLD R2.3 step 8).

Standalone:   python3 scripts/check_chip.py --port /dev/ttyACM0
Self-test:    python3 scripts/check_chip.py --self-test
PlatformIO:   loaded as post: script; runs before every USB upload (not before OTA).
Works with esptool v4 ("Chip is ...", flash_id) and v5 ("Chip type: ...", flash-id).
"""

from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys

EXPECTED_MAC = "48:ca:43:9e:74:b4"

CHIP_RE = re.compile(r"(?:Chip is|Chip type:)\s*(ESP32-S3)\b")
FLASH_RE = re.compile(r"Detected flash size:\s*8MB")
PSRAM_RE = re.compile(r"Embedded PSRAM 8MB")
MAC_RE = re.compile(r"MAC:\s*([0-9a-fA-F:]{17})")


def evaluate(text: str) -> tuple[list[str], str | None]:
    """Return (problems, mac). No problems = safe to flash."""
    problems: list[str] = []
    if not CHIP_RE.search(text):
        problems.append("chip is not an ESP32-S3")
    if not FLASH_RE.search(text):
        problems.append("flash size is not 8MB")
    if not PSRAM_RE.search(text):
        problems.append("no embedded 8MB PSRAM reported")
    m = MAC_RE.search(text)
    return problems, (m.group(1).lower() if m else None)


def esptool_cmd(python: str = sys.executable, package_dir: str | None = None) -> list[str]:
    if package_dir and pathlib.Path(package_dir, "esptool.py").exists():
        return [python, str(pathlib.Path(package_dir, "esptool.py"))]
    probe = subprocess.run([python, "-c", "import esptool"], capture_output=True, check=False)
    if probe.returncode == 0:
        return [python, "-m", "esptool"]
    if shutil.which("uvx"):
        return ["uvx", "--from", "esptool", "esptool"]
    raise SystemExit("check_chip: esptool not found (uv tool install esptool)")


def run_check(cmd: list[str], port: str) -> int:
    text = ""
    for verb in ("flash-id", "flash_id"):  # v5 name first, v4 fallback
        proc = subprocess.run(
            [*cmd, "--port", port, verb], capture_output=True, text=True, check=False
        )
        text = proc.stdout + proc.stderr
        if proc.returncode == 0:
            break
    problems, mac = evaluate(text)
    if problems:
        print(text)
        for p in problems:
            print(f"check_chip: REFUSED - {p}")
        return 1
    print(f"check_chip: OK - ESP32-S3, 8MB flash, 8MB PSRAM, MAC {mac}")
    if mac != EXPECTED_MAC:
        print(f"check_chip: WARNING - MAC {mac} is not the owner's board ({EXPECTED_MAC})")
    return 0


V4_SAMPLE = """Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
MAC: 48:ca:43:9e:74:b4
Detected flash size: 8MB"""
V5_SAMPLE = """Chip type:          ESP32-S3 (QFN56) (revision v0.2)
Features:           Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz, Embedded PSRAM 8MB (AP_3v3)
MAC:                48:ca:43:9e:74:b4
Detected flash size: 8MB"""
WROVER_SAMPLE = """Chip is ESP32-D0WD-V3 (revision v3.0)
Features: WiFi, BT, Dual Core, 240MHz
MAC: 08:3a:f2:00:00:01
Detected flash size: 4MB"""


def self_test() -> int:
    assert evaluate(V4_SAMPLE) == ([], EXPECTED_MAC)
    assert evaluate(V5_SAMPLE) == ([], EXPECTED_MAC)
    problems, _ = evaluate(WROVER_SAMPLE)
    assert len(problems) == 3, problems
    print("check_chip self-test: OK")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    if "--port" not in argv:
        print("usage: check_chip.py --port /dev/ttyACM0 | --self-test")
        return 2
    return run_check(esptool_cmd(), argv[argv.index("--port") + 1])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
else:  # loaded by PlatformIO as an extra script
    Import("env")  # type: ignore[name-defined]  # noqa: F821

    def _before_upload(source, target, env):  # type: ignore[no-untyped-def]  # noqa: ARG001
        if env.subst("$UPLOAD_PROTOCOL") == "espota":
            return
        pkg = env.PioPlatform().get_package_dir("tool-esptoolpy")
        port = env.subst("$UPLOAD_PORT")
        if not port:
            env.AutodetectUploadPort()
            port = env.subst("$UPLOAD_PORT")
        if run_check(esptool_cmd(env.subst("$PYTHONEXE"), pkg), port) != 0:
            env.Exit(1)

    env.AddPreAction("upload", _before_upload)  # type: ignore[name-defined]  # noqa: F821
```

Run: `cd firmware/esp32 && python3 scripts/check_chip.py --self-test`
Expected: `check_chip self-test: OK` (v4 and v5 esptool outputs accepted, a WROVER output refused with 3 problems).

- [ ] **Step 4: Boot log and composition root**

`firmware/esp32/src/net/log.h`:
```cpp
// USB-CDC status log ("[  12345] text"). Safe from any task; never logs secrets.
#pragma once

namespace cq {
namespace net {

void logBegin();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace net
}  // namespace cq
```

`firmware/esp32/src/net/log.cpp`:
```cpp
#include "net/log.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

#include "board_config.h"

namespace cq {
namespace net {

void logBegin() {
  Serial.begin(board::kLogBaud);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {
    vTaskDelay(pdMS_TO_TICKS(10));  // wait up to 2 s for a USB host (boot log), never block forever
  }
}

void logf(const char* fmt, ...) {
  char line[224];
  int n = snprintf(line, sizeof(line), "[%7lu] ", static_cast<unsigned long>(millis()));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof(line) - static_cast<size_t>(n) - 2, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\r';
  line[len++] = '\n';
  Serial.write(reinterpret_cast<const uint8_t*>(line), len);  // one write: lines never mix
}

}  // namespace net
}  // namespace cq
```

`firmware/esp32/src/main.cpp`:
```cpp
// Conqueror ESP32-S3 developer bridge - composition root (bridge LLD R2.6).
// DEVELOPER BRIDGE - NO AUTHENTICATION - home network only. Never in release firmware.
#include <Arduino.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>

#include "board_config.h"
#include "net/log.h"
#include "secrets_guard.h"
#include "version.h"

namespace {

uint32_t g_last_status_ms = 0;

void logBanner() {
  using cq::net::logf;
  logf("%s %s commit %.8s%s built %s", CQ_FW_NAME, CQ_FW_VERSION, CQ_GIT_COMMIT,
       CQ_GIT_DIRTY ? " (dirty)" : "", CQ_BUILD_DATE);
  logf("repo %s", CQ_REPO_URL);
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  logf("chip %s rev %u, %u MHz, flash %lu MB, PSRAM %lu MB, MAC %02x:%02x:%02x:%02x:%02x:%02x",
       ESP.getChipModel(), static_cast<unsigned>(ESP.getChipRevision()),
       static_cast<unsigned>(ESP.getCpuFreqMHz()),
       static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
       static_cast<unsigned long>(ESP.getPsramSize() >> 20), mac[0], mac[1], mac[2], mac[3], mac[4],
       mac[5]);
  const esp_partition_t* running = esp_ota_get_running_partition();
  logf("running from partition %s", running ? running->label : "?");
  logf("DEVELOPER BRIDGE - NO AUTHENTICATION - home network only");
}

void logStatus(uint32_t now_ms) {
  cq::net::logf("status: up %lus heap %lu psram %lu", static_cast<unsigned long>(now_ms / 1000),
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getFreePsram()));
}

}  // namespace

void setup() {
  cq::net::logBegin();
  logBanner();
  if (ESP.getPsramSize() == 0) {
    cq::net::logf("ERROR: no PSRAM - check board_build.arduino.memory_type = qio_opi");
  }
}

void loop() {
  const uint32_t now = millis();
  if (now - g_last_status_ms >= cq::board::kStatusPeriodMs) {
    g_last_status_ms = now;
    logStatus(now);
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
```

- [ ] **Step 5: Local secrets, then build**

Run:
```bash
cd firmware/esp32 && cp include/secrets.example.h include/secrets.h
git check-ignore -v include/secrets.h
```
Expected: `.gitignore:…:firmware/esp32/include/secrets.h	include/secrets.h`. Edit `include/secrets.h`: home SSID/password, an AP password (≥ 8 chars), an OTA password (not `change-me`).

Run: `pio run -e bridge`
Expected: `protogen_check: generated files are fresh`, `stamp_version: <version> commit=<8 hex> dirty=0|1`, then `SUCCESS`; RAM/Flash summary with Flash well under the 3 MB app slot (~0.3 MB at this point). With the unchanged template the build stops at `static_assert … set CQ_OTA_PASSWORD in secrets.h` — that is the intended guard.

- [ ] **Step 6: Confirm the toolchain has WebSocket support in `esp_http_server` (LLD R2.11)**

Run: `grep -rh "define CONFIG_HTTPD_WS_SUPPORT" ~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/ | sort -u`
Expected: `#define CONFIG_HTTPD_WS_SUPPORT 1`. If the line is missing, **stop and ask the owner**: the fallback in the LLD is the links2004 `WebSockets` library on port 80 (Task 6 changes only inside `http_server.cpp`).

- [ ] **Step 7 [HW]: Chip check, first USB flash, boot log**

Connect the board's USB-C directly (shield switch S1 position does not matter for the ESP32's own USB; hold BOOT while plugging in if no port appears). Find the port: Linux `ls /dev/ttyACM*` (expected `/dev/ttyACM0`), macOS `ls /dev/cu.usbmodem*`.

Run: `python3 scripts/check_chip.py --port /dev/ttyACM0`
Expected: `check_chip: OK - ESP32-S3, 8MB flash, 8MB PSRAM, MAC 48:ca:43:9e:74:b4` (another MAC only prints a WARNING). Anything else prints `REFUSED` — do not flash.

Run: `pio run -e bridge -t upload --upload-port /dev/ttyACM0 && pio device monitor -p /dev/ttyACM0 -b 115200`
Expected: the upload starts with the same `check_chip: OK` line (pre-upload hook); after reset the monitor shows:
```
[    2xx] conqueror-bridge <version> commit <8 hex> built 2026-…Z
[    2xx] repo https://github.com/mannuray/conqueror
[    2xx] chip ESP32-S3 rev 2, 240 MHz, flash 8 MB, PSRAM 8 MB, MAC 48:ca:43:9e:74:b4
[    2xx] running from partition app0
[    2xx] DEVELOPER BRIDGE - NO AUTHENTICATION - home network only
[  10xxx] status: up 10s heap 3xxxxx psram 83xxxxx
```
(`rev 2` = esptool's v0.2.) Exit the monitor with `Ctrl-C`.

- [ ] **Step 8: Commit**

```bash
git add .gitignore firmware/esp32/platformio.ini firmware/esp32/partitions_cq8m.csv firmware/esp32/include/board_config.h firmware/esp32/include/secrets.example.h firmware/esp32/include/secrets_guard.h firmware/esp32/scripts firmware/esp32/src
git status --short | grep secrets.h && echo "STOP: secrets.h staged" || true
git commit -m "feat(esp32): ESP32-S3 PlatformIO project, partitions, version stamp, chip check

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
Expected: only `secrets.example.h` and `secrets_guard.h` from `include/` are committed; no `STOP` line.

---

### Task 2: Body link on the ESP32 — parser, frame-aligned TX queue, master handshake, heartbeats, STOP resend

**Files:**
- Create: `firmware/esp32/src/core/frame_parser.h`, `firmware/esp32/src/core/frame_parser.cpp`, `firmware/esp32/src/core/frame_tx.h`, `firmware/esp32/src/core/frame_tx.cpp`, `firmware/esp32/src/core/body_link.h`, `firmware/esp32/src/core/body_link.cpp`, `firmware/esp32/src/net/uart_port.h`, `firmware/esp32/src/net/uart_port.cpp`, `firmware/esp32/src/net/link_task.h`, `firmware/esp32/src/net/link_task.cpp`, `firmware/esp32/scripts/native_paths.py`, `firmware/esp32/test/support/frames.h`, `firmware/esp32/test/support/wire.h`, `firmware/esp32/test/test_frame_parser/test_main.cpp`, `firmware/esp32/test/test_frame_tx/test_main.cpp`, `firmware/esp32/test/test_body_link/test_main.cpp`
- Modify: `firmware/esp32/platformio.ini` (`[env:native]` extra script), `firmware/esp32/src/main.cpp` (start `link_task`)

**Interfaces:**
- Consumes: `cq_protocol.h` (`encode`/`decode`, `crc8`, `FrameType`, `*Msg`, `kProtocolVersion`), `protocol/vectors/frames.json`, `cq::board::*`, `cq::net::logf`.
- Produces:
  - `cq::core::FrameParser` — `template <class F> feed(const uint8_t*, size_t, F&& on_frame)` with `on_frame(const FrameView&)`; `stats()` → `ParserStats{frames, bad_crc, bad_len}`; same resync rules as `conqueror_protocol.framing.FrameParser`.
  - `cq::core::FrameTx` — `push(data, len)`, `pushUrgent(data, len)`, `peek(const uint8_t*&) → len`, `pop()`, `clear()`, `queued()`, `dropped()`; chunks ≤ 64 B, 32 slots, one urgent slot served at the next chunk boundary.
  - `cq::core::BodyLink(FrameTx&)` — `startMaster(now)`, `stopMaster()`, `master()`, `synced()`, `handshake()`, `step(now)`, `onUartBytes(data, len, now)`, `sendDrive(v, w)`, `sendCamJoy(x, y, active)`, `sendCamHome(now, seq&)`, `setOwnerHint(OwnerHintValue)`, `sendStop(now) → seq`, `injectStop(seq, now)`, `stopPending()`, `popEvent(LinkEvent&)`, `hello() → HelloInfo`, `snapshot() → TankSnapshot`, `parserStats()`, `stats()`, `lastRxMs()`; constants `kHeartbeatMs = 100`, `kHelloRetryMs = 1000`, `kUnoSilentMs = 1000`, `kStopResendMs = 50`, `kStopMaxSends = 10`, `kAckTimeoutMs = 200`, `kStatusTimeout = 0xFF`; `LinkEventKind {kUnoEvent, kStopResult, kAckResult, kBodyOnline, kBodyOffline, kResetRequest}`.
  - `cq::net::UartPort` (`begin`, `read`, `txFree`, `write`), `cq::net::LinkCommand {Kind {kWsOpen, kWsClose, kWsText, kOtaStart}, fd, len, text[200]}`, `cq::net::linkTaskStart()`, `cq::net::linkPost(cmd, urgent)`.

- [ ] **Step 1: Native env gets the vectors path**

In `firmware/esp32/platformio.ini`, add to `[env:native]` (after `build_src_flags`):
```ini
extra_scripts = pre:scripts/native_paths.py
```

`firmware/esp32/scripts/native_paths.py`:
```python
"""PlatformIO pre-script for env:native: absolute path of protocol/vectors/frames.json for tests."""

import pathlib

Import("env")  # type: ignore[name-defined]  # noqa: F821 - SCons injects Import/env

vectors = (
    pathlib.Path(env.subst("$PROJECT_DIR")).parents[1] / "protocol" / "vectors" / "frames.json"  # type: ignore[name-defined]  # noqa: F821
)
env.Append(CPPDEFINES=[("CQ_VECTORS_PATH", env.StringifyMacro(str(vectors)))])  # type: ignore[name-defined]  # noqa: F821
```

- [ ] **Step 2: Write the failing tests**

Test helpers (build frames only with the generated codec; drain a `FrameTx` like the UART):

`firmware/esp32/test/support/frames.h`:
```cpp
// Test helper: build frames with the generated codec (tests never hand-code frame bytes).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cq_protocol.h"

namespace cqtest {

template <class Msg>
size_t frame(const Msg& m, uint8_t seq, uint8_t* out) {
  const uint8_t n = cq::proto::encode(m, out + 4);
  out[0] = cq::proto::kSync;
  out[1] = n;
  out[2] = static_cast<uint8_t>(Msg::kType);
  out[3] = seq;
  out[4 + n] = cq::proto::crc8(out + 1, static_cast<uint8_t>(n + 3));
  return n + 5u;
}

}  // namespace cqtest
```

`firmware/esp32/test/support/wire.h`:
```cpp
// Test helper: drain a FrameTx like the UART would and record the frames that went out.
#pragma once

#include <string.h>

#include "core/frame_parser.h"
#include "core/frame_tx.h"

namespace cqtest {

struct Sent {
  uint8_t type;
  uint8_t seq;
  uint8_t len;
  uint8_t payload[48];
};

struct Wire {
  cq::core::FrameParser parser;
  Sent sent[128];
  size_t n = 0;

  void drain(cq::core::FrameTx& tx) {
    const uint8_t* p = nullptr;
    size_t len = 0;
    while ((len = tx.peek(p)) != 0) {
      parser.feed(p, len, [this](const cq::core::FrameView& f) {
        if (n < 128) {
          sent[n].type = f.type;
          sent[n].seq = f.seq;
          sent[n].len = f.len;
          memcpy(sent[n].payload, f.payload, f.len);
          ++n;
        }
      });
      tx.pop();
    }
  }
  size_t count(cq::proto::FrameType t) const {
    size_t c = 0;
    for (size_t i = 0; i < n; ++i) c += sent[i].type == static_cast<uint8_t>(t) ? 1 : 0;
    return c;
  }
  const Sent* last(cq::proto::FrameType t) const {
    for (size_t i = n; i > 0; --i) {
      if (sent[i - 1].type == static_cast<uint8_t>(t)) return &sent[i - 1];
    }
    return nullptr;
  }
  void clear() { n = 0; }
};

}  // namespace cqtest
```

`firmware/esp32/test/test_frame_parser/test_main.cpp`:
```cpp
// FrameParser: protocol/vectors/frames.json + the resync cases of Plan A's Python parser tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "../support/frames.h"
#include "core/frame_parser.h"

using cq::core::FrameParser;
using cq::core::FrameView;

namespace {

struct Seen {
  uint8_t types[64];
  size_t n = 0;
};

void collect(FrameParser& p, const uint8_t* d, size_t len, Seen& s, size_t chunk = 0) {
  if (chunk == 0) chunk = len;
  for (size_t i = 0; i < len; i += chunk) {
    const size_t n = (len - i < chunk) ? len - i : chunk;
    p.feed(d + i, n, [&s](const FrameView& f) {
      if (s.n < sizeof(s.types)) s.types[s.n++] = f.type;
    });
  }
}

size_t stopFrame(uint8_t* out) { return cqtest::frame(cq::proto::StopMsg{1}, 5, out); }  // golden 1
size_t turnFrame(uint8_t* out) {
  return cqtest::frame(cq::proto::TurnMsg{2, 0x81, -900, 40, 0}, 7, out);  // golden 2
}

// Reads every "hex": "aa .." string of protocol/vectors/frames.json.
size_t loadVectors(uint8_t* out, size_t cap, size_t& count, uint8_t* types) {
  FILE* f = fopen(CQ_VECTORS_PATH, "rb");
  if (!f) return 0;
  static char text[200000];
  const size_t len = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[len] = '\0';
  size_t n = 0;
  count = 0;
  for (const char* p = strstr(text, "\"hex\": \""); p; p = strstr(p, "\"hex\": \"")) {
    p += 8;
    types[count++] = static_cast<uint8_t>(strtoul(p + 6, nullptr, 16));  // "aa ll TT"
    while (*p != '"' && n < cap) {
      out[n++] = static_cast<uint8_t>(strtoul(p, const_cast<char**>(&p), 16));
      while (*p == ' ') ++p;
    }
  }
  return n;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_all_vectors_parse_in_order() {
  static uint8_t data[20000];
  uint8_t types[128];
  size_t count = 0;
  const size_t len = loadVectors(data, sizeof(data), count, types);
  TEST_ASSERT_GREATER_THAN(50, static_cast<int>(count));
  const size_t chunks[] = {0, 1, 7};
  for (size_t chunk : chunks) {
    FrameParser p;
    size_t got = 0;
    bool order_ok = true;
    for (size_t i = 0; i < len; i += (chunk ? chunk : len)) {
      const size_t n = chunk && len - i > chunk ? chunk : len - i;
      p.feed(data + i, n, [&](const FrameView& f) {
        if (got >= count || types[got] != f.type) order_ok = false;
        ++got;
      });
    }
    TEST_ASSERT_EQUAL(count, got);
    TEST_ASSERT_TRUE(order_ok);
    TEST_ASSERT_EQUAL(0, p.stats().bad_crc);
  }
}

void test_split_and_noise() {
  uint8_t d[32] = {0x00, 0x13, 0xFF};
  const size_t n = 3 + turnFrame(d + 3);
  FrameParser p;
  Seen s;
  collect(p, d, n, s, 1);
  TEST_ASSERT_EQUAL(1, s.n);
  TEST_ASSERT_EQUAL_HEX8(0x13, s.types[0]);
}

void test_bad_crc_then_good() {
  uint8_t d[32];
  const size_t g = stopFrame(d);
  memcpy(d + g, d, g);
  d[g - 1] ^= 0xFF;  // corrupt the first copy's CRC
  FrameParser p;
  Seen s;
  collect(p, d, 2 * g, s);
  TEST_ASSERT_EQUAL(1, p.stats().bad_crc);
  TEST_ASSERT_EQUAL(1, s.n);
  TEST_ASSERT_EQUAL_HEX8(0x12, s.types[0]);
}

void test_bad_len() {
  const uint8_t d[] = {0xAA, 49, 0x10, 0};
  FrameParser p;
  Seen s;
  collect(p, d, sizeof(d), s);
  TEST_ASSERT_EQUAL(0, s.n);
  TEST_ASSERT_EQUAL(1, p.stats().bad_len);
}

void test_resyncs_after_stray_sync() {
  uint8_t d[32] = {0xAA};
  const size_t n = 1 + stopFrame(d + 1);
  FrameParser p;
  Seen s;
  collect(p, d, n, s);
  TEST_ASSERT_EQUAL(1, s.n);
  TEST_ASSERT_EQUAL(1, p.stats().bad_len);
}

void test_recovers_good_frame_after_truncated_header() {
  const uint8_t junk1[] = {0xAA, 0x05};
  const uint8_t junk2[] = {0xAA, 0x03, 0x10, 0x00, 0x01};
  const uint8_t* junks[] = {junk1, junk2};
  const size_t jl[] = {sizeof(junk1), sizeof(junk2)};
  for (int j = 0; j < 2; ++j) {
    uint8_t d[64];
    memcpy(d, junks[j], jl[j]);
    size_t n = jl[j];
    for (int k = 0; k < 3; ++k) n += stopFrame(d + n);
    FrameParser p;
    Seen s;
    collect(p, d, n, s);
    TEST_ASSERT_EQUAL(3, s.n);
    TEST_ASSERT_EQUAL(1, p.stats().bad_crc);
  }
}

void test_noise_with_many_syncs_then_frames() {
  uint8_t d[96] = {0xAA, 0xAA, 0x00, 0xAA, 49, 0xAA, 0x02, 0xAA};
  size_t n = 8;
  n += stopFrame(d + n);
  n += turnFrame(d + n);
  n += stopFrame(d + n);
  const size_t chunks[] = {n, 1, 3};
  for (size_t chunk : chunks) {
    FrameParser p;
    Seen s;
    collect(p, d, n, s, chunk);
    TEST_ASSERT_EQUAL(3, s.n);
    TEST_ASSERT_EQUAL_HEX8(0x12, s.types[0]);
    TEST_ASSERT_EQUAL_HEX8(0x13, s.types[1]);
    TEST_ASSERT_EQUAL_HEX8(0x12, s.types[2]);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_all_vectors_parse_in_order);
  RUN_TEST(test_split_and_noise);
  RUN_TEST(test_bad_crc_then_good);
  RUN_TEST(test_bad_len);
  RUN_TEST(test_resyncs_after_stray_sync);
  RUN_TEST(test_recovers_good_frame_after_truncated_header);
  RUN_TEST(test_noise_with_many_syncs_then_frames);
  return UNITY_END();
}
```

`firmware/esp32/test/test_frame_tx/test_main.cpp`:
```cpp
// FrameTx: whole chunks only, urgent STOP at the next chunk boundary, bounded, no heap.
#include <string.h>
#include <unity.h>

#include "core/frame_tx.h"

using cq::core::FrameTx;

namespace {
FrameTx* g_tx = nullptr;
const uint8_t kA[] = {0xAA, 1, 2, 3};
const uint8_t kB[] = {0xAA, 4, 5};
const uint8_t kS[] = {0xAA, 9};
}  // namespace

void setUp() {
  static FrameTx tx;
  tx = FrameTx();
  g_tx = &tx;
}
void tearDown() {}

void test_fifo_whole_chunks() {
  TEST_ASSERT_TRUE(g_tx->push(kA, sizeof(kA)));
  TEST_ASSERT_TRUE(g_tx->push(kB, sizeof(kB)));
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));
  TEST_ASSERT_EQUAL_MEMORY(kA, p, sizeof(kA));
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kB), g_tx->peek(p));
  TEST_ASSERT_EQUAL_MEMORY(kB, p, sizeof(kB));
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));
}

void test_urgent_goes_first_when_idle() {
  g_tx->push(kA, sizeof(kA));
  g_tx->pushUrgent(kS, sizeof(kS));
  const uint8_t* p = nullptr;
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));
  TEST_ASSERT_EQUAL_MEMORY(kS, p, sizeof(kS));
}

void test_urgent_waits_for_chunk_boundary() {
  g_tx->push(kA, sizeof(kA));
  g_tx->push(kB, sizeof(kB));
  const uint8_t* p = nullptr;
  g_tx->peek(p);  // A is being written
  g_tx->pushUrgent(kS, sizeof(kS));
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));  // still A: never cut a chunk
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kS), g_tx->peek(p));  // STOP before B
  g_tx->pop();
  TEST_ASSERT_EQUAL(sizeof(kB), g_tx->peek(p));
}

void test_full_queue_drops_and_counts() {
  for (size_t i = 0; i < FrameTx::kSlots; ++i) TEST_ASSERT_TRUE(g_tx->push(kA, sizeof(kA)));
  TEST_ASSERT_FALSE(g_tx->push(kA, sizeof(kA)));
  uint8_t big[FrameTx::kMaxChunk + 1] = {};
  TEST_ASSERT_FALSE(g_tx->pushUrgent(big, sizeof(big)));
  TEST_ASSERT_EQUAL(2, g_tx->dropped());
}

void test_clear_keeps_chunk_in_flight() {
  g_tx->push(kA, sizeof(kA));
  g_tx->push(kB, sizeof(kB));
  const uint8_t* p = nullptr;
  g_tx->peek(p);
  g_tx->clear();
  TEST_ASSERT_EQUAL(sizeof(kA), g_tx->peek(p));
  g_tx->pop();
  TEST_ASSERT_EQUAL(0, g_tx->peek(p));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fifo_whole_chunks);
  RUN_TEST(test_urgent_goes_first_when_idle);
  RUN_TEST(test_urgent_waits_for_chunk_boundary);
  RUN_TEST(test_full_queue_drops_and_counts);
  RUN_TEST(test_clear_keeps_chunk_in_flight);
  return UNITY_END();
}
```

`firmware/esp32/test/test_body_link/test_main.cpp`:
```cpp
// BodyLink: master handshake, heartbeats, STOP resend until ACK, telemetry snapshot, events.
#include <unity.h>

#include <new>

#include "../support/frames.h"
#include "../support/wire.h"
#include "core/body_link.h"

using cq::core::BodyLink;
using cq::core::FrameTx;
using cq::core::HandshakeState;
using cq::core::LinkEvent;
using cq::core::LinkEventKind;
namespace proto = cq::proto;
using FT = cq::proto::FrameType;

namespace {

FrameTx* tx = nullptr;
BodyLink* bl = nullptr;
cqtest::Wire* wire = nullptr;

void unoSends(const uint8_t* d, size_t n, uint32_t now) { bl->onUartBytes(d, n, now); }

void hello(uint8_t proto_version, uint32_t now, uint8_t seq = 1) {
  uint8_t f[64];
  const size_t n =
      cqtest::frame(proto::HelloMsg{0x0100, proto_version, 0, 1, 0x1a2b3c4dUL}, seq, f);
  unoSends(f, n, now);
}

void ack(uint8_t ack_seq, uint8_t status, uint32_t now) {
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::AckMsg{ack_seq, status}, 9, f);
  unoSends(f, n, now);
}

void run(uint32_t from, uint32_t to) {
  for (uint32_t t = from; t <= to; t += 2) {  // link_task: step + drain every 2 ms
    bl->step(t);
    wire->drain(*tx);
  }
}

bool popKind(LinkEventKind k, LinkEvent& out) {
  LinkEvent ev{};
  while (bl->popEvent(ev)) {
    if (ev.kind == k) {
      out = ev;
      return true;
    }
  }
  return false;
}

}  // namespace

void setUp() {
  static FrameTx s_tx;
  static cqtest::Wire s_wire;
  s_tx = FrameTx();
  s_wire = cqtest::Wire();
  tx = &s_tx;
  wire = &s_wire;
  alignas(BodyLink) static unsigned char s_mem[sizeof(BodyLink)];
  bl = new (s_mem) BodyLink(*tx);  // fresh object per test, no heap
}
void tearDown() {}

void test_start_master_sends_heartbeat_and_hello_req() {
  bl->startMaster(1000);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kHeartbeat));
  TEST_ASSERT_EQUAL(1, wire->count(FT::kHelloReq));
  TEST_ASSERT_EQUAL(static_cast<int>(HandshakeState::kWaitHello),
                    static_cast<int>(bl->handshake()));
}

void test_heartbeat_every_100ms() {
  bl->startMaster(0);
  run(2, 1000);
  TEST_ASSERT_EQUAL(11, wire->count(FT::kHeartbeat));  // t = 0, 100, ..., 1000
}

void test_hello_req_repeats_every_second_until_hello() {
  bl->startMaster(0);
  run(2, 2500);
  TEST_ASSERT_EQUAL(3, wire->count(FT::kHelloReq));  // 0, 1000, 2000
}

void test_handshake_acks_matching_proto_and_sends_owner_hint() {
  bl->startMaster(0);
  bl->setOwnerHint(proto::OwnerHintValue::kNone);
  hello(proto::kProtocolVersion, 50);
  wire->drain(*tx);
  const cqtest::Sent* a = wire->last(FT::kHelloAck);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL(proto::kProtocolVersion, a->payload[0]);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kOwnerHint));
  TEST_ASSERT_EQUAL(0, wire->count(FT::kEnvelope));  // R2.14 #5: never an ENVELOPE
  TEST_ASSERT_TRUE(bl->synced());
  TEST_ASSERT_EQUAL(0x1a2b3c4dUL, bl->hello().commit);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kBodyOnline, ev));
}

void test_handshake_rejects_wrong_proto() {
  bl->startMaster(0);
  hello(1, 50);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(0, wire->count(FT::kHelloAck));
  TEST_ASSERT_FALSE(bl->synced());
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kBodyOffline, ev));
  TEST_ASSERT_EQUAL(1, ev.code);
  TEST_ASSERT_EQUAL(1, ev.a);
  TEST_ASSERT_FALSE(bl->sendDrive(10, 0));  // motion refused while incompatible
}

void test_uno_reboot_hello_is_acked_again() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  hello(proto::kProtocolVersion, 500, 2);  // Uno rebooted: HELLO again
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(2, wire->count(FT::kHelloAck));
}

void test_silent_uno_restarts_handshake() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  wire->drain(*tx);
  wire->clear();
  run(12, 1100);  // no Uno bytes since t = 10
  TEST_ASSERT_FALSE(bl->synced());
  TEST_ASSERT_GREATER_THAN(0, static_cast<int>(wire->count(FT::kHelloReq)));
}

void test_not_master_never_sends_heartbeat_or_hello_ack() {
  hello(proto::kProtocolVersion, 10);  // bench is master (passthrough)
  run(0, 1000);
  TEST_ASSERT_EQUAL(0, wire->count(FT::kHeartbeat));
  TEST_ASSERT_EQUAL(0, wire->count(FT::kHelloAck));
  TEST_ASSERT_TRUE(bl->hello().seen);  // HELLO info still recorded for /info
}

void test_drive_only_when_synced() {
  bl->startMaster(0);
  TEST_ASSERT_FALSE(bl->sendDrive(40, 0));
  hello(proto::kProtocolVersion, 10);
  TEST_ASSERT_TRUE(bl->sendDrive(40, -20));
  wire->drain(*tx);
  const cqtest::Sent* d = wire->last(FT::kDrive);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::Source::kHuman), d->payload[0]);
  TEST_ASSERT_EQUAL(40, static_cast<int8_t>(d->payload[1]));
  TEST_ASSERT_EQUAL(-20, static_cast<int8_t>(d->payload[2]));
}

void test_bridge_never_invents_drive() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  bl->sendDrive(50, 0);
  for (uint32_t t = 12; t <= 5000; t += 2) {
    if (t % 40 == 0) {
      uint8_t f[64];
      const size_t n = cqtest::frame(proto::TelemFastMsg{}, 3, f);  // keep the link alive
      unoSends(f, n, t);
    }
    bl->step(t);
  }
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(1, wire->count(FT::kDrive));  // exactly the one client message
  TEST_ASSERT_EQUAL(0, wire->count(FT::kCamJoy));
}

void test_stop_resend_until_ack() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  const uint8_t seq = bl->sendStop(20);
  run(22, 130);  // sends at 20, 70, 120
  TEST_ASSERT_EQUAL(3, wire->count(FT::kStop));
  TEST_ASSERT_EQUAL(seq, wire->last(FT::kStop)->seq);
  ack(seq, 0, 131);
  run(132, 400);
  TEST_ASSERT_EQUAL(3, wire->count(FT::kStop));
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kStopResult, ev));
  TEST_ASSERT_EQUAL(seq, ev.seq);
  TEST_ASSERT_EQUAL(0, ev.status);
}

void test_stop_gives_up_after_ten_sends() {
  bl->sendStop(0);  // works without being master (passthrough / not synced)
  run(2, 1000);
  TEST_ASSERT_EQUAL(10, wire->count(FT::kStop));
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kStopResult, ev));
  TEST_ASSERT_EQUAL(cq::core::kStatusTimeout, ev.status);
}

void test_inject_stop_uses_callers_seq() {
  bl->injectStop(200, 0);
  wire->drain(*tx);
  TEST_ASSERT_EQUAL(200, wire->last(FT::kStop)->seq);
}

void test_cam_home_ack_and_timeout() {
  bl->startMaster(0);
  hello(proto::kProtocolVersion, 10);
  uint8_t seq = 0;
  TEST_ASSERT_TRUE(bl->sendCamHome(20, seq));
  ack(seq, 0, 30);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(0, ev.status);
  TEST_ASSERT_TRUE(bl->sendCamHome(40, seq));
  run(42, 300);
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kAckResult, ev));
  TEST_ASSERT_EQUAL(cq::core::kStatusTimeout, ev.status);
}

void test_telemetry_snapshot_and_events() {
  proto::TelemFastMsg tf{};
  tf.yaw = 9000;
  tf.dist_mm = 420;
  uint8_t f[64];
  size_t n = cqtest::frame(tf, 1, f);
  unoSends(f, n, 5);
  n = cqtest::frame(proto::EventMsg{static_cast<uint8_t>(proto::EventKind::kDeadmanStop), 0, 0}, 2,
                    f);
  unoSends(f, n, 6);
  TEST_ASSERT_TRUE(bl->snapshot().fast_valid);
  TEST_ASSERT_EQUAL(9000, bl->snapshot().fast.yaw);
  LinkEvent ev{};
  TEST_ASSERT_TRUE(popKind(LinkEventKind::kUnoEvent, ev));
  TEST_ASSERT_EQUAL(static_cast<int>(proto::EventKind::kDeadmanStop), ev.code);
}

void test_wifi_reset_request_answered_unavailable() {
  bl->startMaster(0);
  uint8_t f[16];
  const size_t n = cqtest::frame(proto::WifiResetReqMsg{}, 77, f);
  unoSends(f, n, 5);
  wire->drain(*tx);
  const cqtest::Sent* a = wire->last(FT::kEspAck);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL(77, a->payload[0]);
  TEST_ASSERT_EQUAL(static_cast<uint8_t>(proto::AckStatus::kUnavailable), a->payload[1]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_start_master_sends_heartbeat_and_hello_req);
  RUN_TEST(test_heartbeat_every_100ms);
  RUN_TEST(test_hello_req_repeats_every_second_until_hello);
  RUN_TEST(test_handshake_acks_matching_proto_and_sends_owner_hint);
  RUN_TEST(test_handshake_rejects_wrong_proto);
  RUN_TEST(test_uno_reboot_hello_is_acked_again);
  RUN_TEST(test_silent_uno_restarts_handshake);
  RUN_TEST(test_not_master_never_sends_heartbeat_or_hello_ack);
  RUN_TEST(test_drive_only_when_synced);
  RUN_TEST(test_bridge_never_invents_drive);
  RUN_TEST(test_stop_resend_until_ack);
  RUN_TEST(test_stop_gives_up_after_ten_sends);
  RUN_TEST(test_inject_stop_uses_callers_seq);
  RUN_TEST(test_cam_home_ack_and_timeout);
  RUN_TEST(test_telemetry_snapshot_and_events);
  RUN_TEST(test_wifi_reset_request_answered_unavailable);
  return UNITY_END();
}
```

Run: `cd firmware/esp32 && pio test -e native`
Expected: build errors — `core/frame_parser.h`, `core/frame_tx.h`, `core/body_link.h` not found.

- [ ] **Step 3: Implement the parser**

`firmware/esp32/src/core/frame_parser.h`:
```cpp
// Resyncing body-link frame parser (bridge LLD R2.6). Same frames, order and counters as Plan A's
// conqueror_protocol.framing.FrameParser: after a bad length the parser hunts for SYNC again; after a
// bad CRC the bytes received since the false SYNC are re-scanned, so a good frame hidden behind a
// bad header is not lost. Pure C++ (host-tested on protocol/vectors/).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cq_protocol.h"

namespace cq {
namespace core {

struct FrameView {
  uint8_t type;
  uint8_t seq;
  uint8_t len;
  const uint8_t* payload;  // valid only inside the on_frame callback
};

struct ParserStats {
  uint32_t frames = 0;
  uint32_t bad_crc = 0;
  uint32_t bad_len = 0;
};

class FrameParser {
 public:
  // Calls on_frame(const FrameView&) for every good frame, in order.
  template <class OnFrame>
  void feed(const uint8_t* data, size_t len, OnFrame&& on_frame) {
    for (size_t i = 0; i < len; ++i) feedByte(data[i], on_frame);
  }
  const ParserStats& stats() const { return m_stats; }
  void reset();

 private:
  enum class State : uint8_t { kSync, kLen, kType, kSeq, kPayload, kCrc };
  enum class Step : uint8_t { kNone, kFrame, kReplay };
  static constexpr size_t kRawMax = proto::kMaxPayload + 4;  // len type seq payload crc
  static constexpr size_t kWorkMax = kRawMax + 1;

  template <class OnFrame>
  void feedByte(uint8_t b, OnFrame& on_frame) {
    uint8_t work[kWorkMax];
    size_t n = 0;
    size_t pos = 0;
    work[n++] = b;
    while (pos < n) {
      const Step s = step(work[pos++]);
      if (s == Step::kFrame) {
        const FrameView v{m_type, m_seq, m_len, m_buf};
        on_frame(v);
      } else if (s == Step::kReplay) {
        // replay = bytes after the next SYNC in the false frame; always shorter than what was
        // consumed since that SYNC, so the loop terminates and fits kWorkMax.
        uint8_t rest[kWorkMax];
        const size_t rest_n = n - pos;
        for (size_t i = 0; i < rest_n; ++i) rest[i] = work[pos + i];
        n = 0;
        for (size_t i = 0; i < m_replay_n; ++i) work[n++] = m_replay[i];
        for (size_t i = 0; i < rest_n && n < kWorkMax; ++i) work[n++] = rest[i];
        pos = 0;
      }
    }
  }
  Step step(uint8_t b);

  State m_state = State::kSync;
  uint8_t m_len = 0;
  uint8_t m_type = 0;
  uint8_t m_seq = 0;
  uint8_t m_got = 0;
  uint8_t m_buf[proto::kMaxPayload] = {};
  uint8_t m_raw[kRawMax] = {};  // every byte since the (possibly false) SYNC
  size_t m_raw_n = 0;
  uint8_t m_replay[kRawMax] = {};
  size_t m_replay_n = 0;
  ParserStats m_stats;
};

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/frame_parser.cpp`:
```cpp
#include "core/frame_parser.h"

namespace cq {
namespace core {

void FrameParser::reset() {
  m_state = State::kSync;
  m_raw_n = 0;
  m_replay_n = 0;
}

FrameParser::Step FrameParser::step(uint8_t b) {
  if (m_state == State::kSync) {
    if (b == proto::kSync) {
      m_state = State::kLen;
      m_raw_n = 0;
    }
    return Step::kNone;
  }
  if (m_raw_n < kRawMax) m_raw[m_raw_n++] = b;
  switch (m_state) {
    case State::kLen:
      if (b > proto::kMaxPayload) {
        ++m_stats.bad_len;
        if (b == proto::kSync) {
          m_raw_n = 0;  // treat as a fresh SYNC; stay in kLen
        } else {
          m_state = State::kSync;
        }
        return Step::kNone;
      }
      m_len = b;
      m_state = State::kType;
      return Step::kNone;
    case State::kType:
      m_type = b;
      m_state = State::kSeq;
      return Step::kNone;
    case State::kSeq:
      m_seq = b;
      m_got = 0;
      m_state = m_len ? State::kPayload : State::kCrc;
      return Step::kNone;
    case State::kPayload:
      m_buf[m_got++] = b;
      if (m_got == m_len) m_state = State::kCrc;
      return Step::kNone;
    case State::kCrc:
    default: {
      m_state = State::kSync;
      const uint8_t head[3] = {m_len, m_type, m_seq};
      const uint8_t expected = proto::crc8(m_buf, m_len, proto::crc8(head, 3));
      if (b == expected) {
        ++m_stats.frames;
        return Step::kFrame;
      }
      ++m_stats.bad_crc;
      size_t idx = 0;
      while (idx < m_raw_n && m_raw[idx] != proto::kSync) ++idx;
      if (idx >= m_raw_n) {
        m_raw_n = 0;
        return Step::kNone;
      }
      m_replay_n = 0;
      for (size_t i = idx + 1; i < m_raw_n; ++i) m_replay[m_replay_n++] = m_raw[i];
      m_raw_n = 0;
      m_state = State::kLen;
      return Step::kReplay;
    }
  }
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 4: Implement the whole-chunk TX queue**

`firmware/esp32/src/core/frame_tx.h`:
```cpp
// Whole-chunk TX queue in front of the Uno UART (bridge LLD R2.8). Every writer hands over complete
// chunks (one frame, or one run of raw passthrough bytes); the single drainer (link_task) writes a
// chunk only when the UART can take all of it, so chunks never interleave mid-frame. One urgent
// slot (STOP) goes out at the next chunk boundary, ahead of queued chunks. Pure C++, no heap.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace core {

class FrameTx {
 public:
  static constexpr size_t kMaxChunk = 64;  // > largest frame (53 B)
  static constexpr size_t kSlots = 32;

  bool push(const uint8_t* data, size_t len);        // false (and counted) when full or too long
  bool pushUrgent(const uint8_t* data, size_t len);  // replaces a not-yet-started urgent chunk
  size_t peek(const uint8_t*& data);                 // next chunk (0 = empty); stays until pop()
  void pop();                                        // the peeked chunk was written whole
  void clear();                                      // drop everything not yet peeked
  size_t queued() const { return m_count + (m_urgent_len ? 1 : 0); }
  uint32_t dropped() const { return m_dropped; }

 private:
  enum class Inflight : uint8_t { kNone, kQueue, kUrgent };
  uint8_t m_data[kSlots][kMaxChunk] = {};
  uint8_t m_len[kSlots] = {};
  size_t m_head = 0;
  size_t m_count = 0;
  uint8_t m_urgent[kMaxChunk] = {};
  size_t m_urgent_len = 0;
  Inflight m_inflight = Inflight::kNone;
  uint32_t m_dropped = 0;
};

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/frame_tx.cpp`:
```cpp
#include "core/frame_tx.h"

#include <string.h>

namespace cq {
namespace core {

bool FrameTx::push(const uint8_t* data, size_t len) {
  if (len == 0 || len > kMaxChunk || m_count == kSlots) {
    ++m_dropped;
    return false;
  }
  const size_t tail = (m_head + m_count) % kSlots;
  memcpy(m_data[tail], data, len);
  m_len[tail] = static_cast<uint8_t>(len);
  ++m_count;
  return true;
}

bool FrameTx::pushUrgent(const uint8_t* data, size_t len) {
  if (len == 0 || len > kMaxChunk || m_inflight == Inflight::kUrgent) {
    ++m_dropped;
    return false;
  }
  memcpy(m_urgent, data, len);
  m_urgent_len = len;
  return true;
}

size_t FrameTx::peek(const uint8_t*& data) {
  if (m_inflight == Inflight::kNone) {
    if (m_urgent_len) {
      m_inflight = Inflight::kUrgent;
    } else if (m_count) {
      m_inflight = Inflight::kQueue;
    } else {
      return 0;
    }
  }
  if (m_inflight == Inflight::kUrgent) {
    data = m_urgent;
    return m_urgent_len;
  }
  data = m_data[m_head];
  return m_len[m_head];
}

void FrameTx::pop() {
  if (m_inflight == Inflight::kUrgent) {
    m_urgent_len = 0;
  } else if (m_inflight == Inflight::kQueue) {
    m_head = (m_head + 1) % kSlots;
    --m_count;
  }
  m_inflight = Inflight::kNone;
}

void FrameTx::clear() {
  if (m_inflight == Inflight::kQueue) {
    m_count = 1;  // keep the chunk being written
  } else {
    m_count = 0;
  }
  if (m_inflight != Inflight::kUrgent) m_urgent_len = 0;
}

}  // namespace core
}  // namespace cq
```

- [ ] **Step 5: Implement the body link master**

`firmware/esp32/src/core/body_link.h`:
```cpp
// The ESP32 side of the body link when the ESP32 is the master (bridge LLD R2.6-R2.8).
// Master: HEARTBEAT every 100 ms; HELLO_REQ at start and every 1 s until a HELLO with
// proto == kProtocolVersion, then HELLO_ACK + OWNER_HINT (no ENVELOPE - R2.14 #5). STOP is resent
// every 50 ms until the Uno's ACK (max 10 sends). Uno frames are always parsed (also during TCP
// passthrough) to keep the snapshot, HELLO info and events for the web clients.
// It never originates DRIVE/CAM_JOY: those are sent 1:1 for client messages. Pure C++.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/frame_parser.h"
#include "core/frame_tx.h"
#include "cq_protocol.h"

namespace cq {
namespace core {

constexpr uint32_t kHeartbeatMs = 100;
constexpr uint32_t kHelloRetryMs = 1000;
constexpr uint32_t kUnoSilentMs = 1000;  // synced Uno silent this long -> re-handshake
constexpr uint32_t kStopResendMs = 50;
constexpr uint8_t kStopMaxSends = 10;
constexpr uint32_t kAckTimeoutMs = 200;
constexpr uint8_t kStatusTimeout = 0xFF;  // LinkEvent.status: no ACK in time

enum class HandshakeState : uint8_t { kIdle, kWaitHello, kSynced, kIncompatible };

struct HelloInfo {
  bool seen = false;
  uint16_t fw = 0;
  uint8_t proto = 0;
  uint8_t caps = 0;
  uint8_t reset_reason = 0;
  uint32_t commit = 0;
};

struct TankSnapshot {
  bool fast_valid = false;
  bool slow_valid = false;
  uint32_t fast_count = 0;  // TELEM_FAST frames seen (tf is sent for every 2nd)
  proto::TelemFastMsg fast{};
  proto::TelemSlowMsg slow{};
};

enum class LinkEventKind : uint8_t {
  kUnoEvent,      // EVENT frame: code, a, b
  kStopResult,    // seq, status (ack_status index or kStatusTimeout)
  kAckResult,     // CAM_HOME: seq, status
  kBodyOnline,    // HELLO accepted
  kBodyOffline,   // code 0 = silent, 1 = incompatible protocol (a = Uno proto)
  kResetRequest,  // code = frame type (WIFI/ADMIN_RESET_REQ), answered UNAVAILABLE
};

struct LinkEvent {
  LinkEventKind kind;
  uint8_t code;
  uint8_t seq;
  uint8_t status;
  int16_t a;
  int16_t b;
};

struct LinkStats {
  uint32_t frames_tx = 0;
  uint32_t frames_rx = 0;
};

class BodyLink {
 public:
  explicit BodyLink(FrameTx& tx) : m_tx(tx) {}

  void startMaster(uint32_t now_ms);  // HEARTBEAT + HELLO_REQ now
  void stopMaster();                  // no more heartbeats; snapshot/events keep flowing
  bool master() const { return m_master; }
  bool synced() const { return m_master && m_hs == HandshakeState::kSynced; }
  HandshakeState handshake() const { return m_hs; }

  void step(uint32_t now_ms);
  void onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms);

  // Client commands (master + synced only; false = not sent).
  bool sendDrive(int8_t v, int8_t w);
  bool sendCamJoy(int8_t x, int8_t y, bool active);
  bool sendCamHome(uint32_t now_ms, uint8_t& seq_out);
  void setOwnerHint(proto::OwnerHintValue hint);  // remembered; sent now if synced, and after HELLO

  // STOP {src HUMAN}: master -> own seq; passthrough -> injectStop with the caller's seq.
  // Both use the urgent slot and are resent until the ACK. Returns the seq used.
  uint8_t sendStop(uint32_t now_ms);
  void injectStop(uint8_t seq, uint32_t now_ms);
  bool stopPending() const { return m_stop_sends != 0; }

  bool popEvent(LinkEvent& ev);
  const HelloInfo& hello() const { return m_hello; }
  const TankSnapshot& snapshot() const { return m_snap; }
  const ParserStats& parserStats() const { return m_parser.stats(); }
  const LinkStats& stats() const { return m_stats; }
  uint32_t lastRxMs() const { return m_last_rx_ms; }

 private:
  template <class Msg>
  bool emit(const Msg& m, uint8_t seq, bool urgent);
  template <class Msg>
  bool emitNext(const Msg& m) {
    return emit(m, m_next_seq++, false);
  }
  void onFrame(const FrameView& f, uint32_t now_ms);
  void onHello(const proto::HelloMsg& h);
  void pushEvent(const LinkEvent& ev);
  void sendStopFrame();

  FrameTx& m_tx;
  FrameParser m_parser;
  bool m_master = false;
  HandshakeState m_hs = HandshakeState::kIdle;
  uint8_t m_next_seq = 0;
  uint32_t m_last_hb_ms = 0;
  uint32_t m_last_req_ms = 0;
  uint32_t m_last_rx_ms = 0;
  bool m_incompatible_reported = false;
  proto::OwnerHintValue m_hint = proto::OwnerHintValue::kNone;
  uint8_t m_stop_seq = 0;
  uint8_t m_stop_sends = 0;
  uint32_t m_stop_last_ms = 0;
  uint8_t m_cam_seq = 0;
  bool m_cam_pending = false;
  uint32_t m_cam_sent_ms = 0;
  HelloInfo m_hello;
  TankSnapshot m_snap;
  LinkStats m_stats;
  static constexpr size_t kEventSlots = 16;
  LinkEvent m_events[kEventSlots] = {};
  size_t m_ev_head = 0;
  size_t m_ev_count = 0;
};

}  // namespace core
}  // namespace cq
```

`firmware/esp32/src/core/body_link.cpp`:
```cpp
#include "core/body_link.h"

namespace cq {
namespace core {

template <class Msg>
bool BodyLink::emit(const Msg& m, uint8_t seq, bool urgent) {
  uint8_t frame[proto::kMaxPayload + 5];
  const uint8_t n = proto::encode(m, frame + 4);
  if (n == proto::kEncodeError || n > proto::kMaxPayload) return false;
  frame[0] = proto::kSync;
  frame[1] = n;
  frame[2] = static_cast<uint8_t>(Msg::kType);
  frame[3] = seq;
  frame[4 + n] = proto::crc8(frame + 1, static_cast<uint8_t>(n + 3));
  const bool ok = urgent ? m_tx.pushUrgent(frame, n + 5u) : m_tx.push(frame, n + 5u);
  if (ok) ++m_stats.frames_tx;
  return ok;
}

void BodyLink::startMaster(uint32_t now_ms) {
  m_master = true;
  m_hs = HandshakeState::kWaitHello;
  m_incompatible_reported = false;
  m_last_hb_ms = now_ms;
  m_last_req_ms = now_ms;
  emitNext(proto::HeartbeatMsg{now_ms});
  emitNext(proto::HelloReqMsg{});
}

void BodyLink::stopMaster() {
  if (m_master && m_hs == HandshakeState::kSynced) {
    pushEvent({LinkEventKind::kBodyOffline, 0, 0, 0, 0, 0});
  }
  m_master = false;
  m_hs = HandshakeState::kIdle;
  m_cam_pending = false;
}

void BodyLink::step(uint32_t now_ms) {
  if (m_master) {
    if (now_ms - m_last_hb_ms >= kHeartbeatMs) {
      m_last_hb_ms = now_ms;
      emitNext(proto::HeartbeatMsg{now_ms});
    }
    if (m_hs == HandshakeState::kSynced && now_ms - m_last_rx_ms >= kUnoSilentMs) {
      m_hs = HandshakeState::kWaitHello;  // Uno gone or rebooting: start over with HELLO_REQ
      pushEvent({LinkEventKind::kBodyOffline, 0, 0, 0, 0, 0});
      m_last_req_ms = now_ms - kHelloRetryMs;
    }
    if (m_hs != HandshakeState::kSynced && now_ms - m_last_req_ms >= kHelloRetryMs) {
      m_last_req_ms = now_ms;
      emitNext(proto::HelloReqMsg{});
    }
  }
  if (m_stop_sends != 0 && now_ms - m_stop_last_ms >= kStopResendMs) {
    if (m_stop_sends >= kStopMaxSends) {
      m_stop_sends = 0;
      pushEvent({LinkEventKind::kStopResult, 0, m_stop_seq, kStatusTimeout, 0, 0});
    } else {
      m_stop_last_ms = now_ms;
      sendStopFrame();
    }
  }
  if (m_cam_pending && now_ms - m_cam_sent_ms >= kAckTimeoutMs) {
    m_cam_pending = false;
    pushEvent({LinkEventKind::kAckResult, 0, m_cam_seq, kStatusTimeout, 0, 0});
  }
}

void BodyLink::onUartBytes(const uint8_t* data, size_t len, uint32_t now_ms) {
  m_parser.feed(data, len, [this, now_ms](const FrameView& f) { onFrame(f, now_ms); });
}

void BodyLink::onFrame(const FrameView& f, uint32_t now_ms) {
  m_last_rx_ms = now_ms;
  ++m_stats.frames_rx;
  switch (static_cast<proto::FrameType>(f.type)) {
    case proto::FrameType::kHello: {
      proto::HelloMsg h{};
      if (proto::decode(f.payload, f.len, h)) onHello(h);
      break;
    }
    case proto::FrameType::kAck: {
      proto::AckMsg a{};
      if (!proto::decode(f.payload, f.len, a)) break;
      if (m_stop_sends != 0 && a.ack_seq == m_stop_seq) {
        m_stop_sends = 0;
        pushEvent({LinkEventKind::kStopResult, 0, a.ack_seq, a.status, 0, 0});
      } else if (m_cam_pending && a.ack_seq == m_cam_seq) {
        m_cam_pending = false;
        pushEvent({LinkEventKind::kAckResult, 0, a.ack_seq, a.status, 0, 0});
      }
      break;
    }
    case proto::FrameType::kTelemFast:
      if (proto::decode(f.payload, f.len, m_snap.fast)) {
        m_snap.fast_valid = true;
        ++m_snap.fast_count;
      }
      break;
    case proto::FrameType::kTelemSlow:
      if (proto::decode(f.payload, f.len, m_snap.slow)) m_snap.slow_valid = true;
      break;
    case proto::FrameType::kEvent: {
      proto::EventMsg e{};
      if (proto::decode(f.payload, f.len, e)) {
        pushEvent({LinkEventKind::kUnoEvent, e.kind, f.seq, 0, e.a, e.b});
      }
      break;
    }
    case proto::FrameType::kWifiResetReq:
    case proto::FrameType::kAdminResetReq:
      if (m_master) {
        const proto::EspAckMsg ack{f.seq, static_cast<uint8_t>(proto::AckStatus::kUnavailable)};
        emitNext(ack);
        pushEvent({LinkEventKind::kResetRequest, f.type, f.seq, 0, 0, 0});
      }
      break;
    default:
      break;  // SETTING_VALUE, CAL_*: only the bench uses them (passthrough)
  }
}

void BodyLink::onHello(const proto::HelloMsg& h) {
  m_hello = HelloInfo{true, h.fw, h.proto, h.caps, h.reset_reason, h.commit};
  if (!m_master) return;  // bench is master: it answers its own HELLO
  if (h.proto != proto::kProtocolVersion) {
    m_hs = HandshakeState::kIncompatible;
    if (!m_incompatible_reported) {
      m_incompatible_reported = true;
      pushEvent({LinkEventKind::kBodyOffline, 1, 0, 0, h.proto, 0});
    }
    return;
  }
  const bool was_synced = m_hs == HandshakeState::kSynced;
  emitNext(proto::HelloAckMsg{proto::kProtocolVersion});
  emitNext(proto::OwnerHintMsg{static_cast<uint8_t>(m_hint)});
  m_hs = HandshakeState::kSynced;
  m_incompatible_reported = false;
  if (!was_synced) pushEvent({LinkEventKind::kBodyOnline, 0, 0, 0, 0, 0});
}

bool BodyLink::sendDrive(int8_t v, int8_t w) {
  if (!synced()) return false;
  return emitNext(proto::DriveMsg{static_cast<uint8_t>(proto::Source::kHuman), v, w});
}

bool BodyLink::sendCamJoy(int8_t x, int8_t y, bool active) {
  if (!synced()) return false;
  return emitNext(proto::CamJoyMsg{x, y, static_cast<uint8_t>(active ? 1 : 0)});
}

bool BodyLink::sendCamHome(uint32_t now_ms, uint8_t& seq_out) {
  if (!synced()) return false;
  seq_out = m_next_seq;
  if (!emitNext(proto::CamHomeMsg{})) return false;
  m_cam_seq = seq_out;
  m_cam_pending = true;
  m_cam_sent_ms = now_ms;
  return true;
}

void BodyLink::setOwnerHint(proto::OwnerHintValue hint) {
  m_hint = hint;
  if (synced()) emitNext(proto::OwnerHintMsg{static_cast<uint8_t>(hint)});
}

uint8_t BodyLink::sendStop(uint32_t now_ms) {
  const uint8_t seq = m_next_seq++;
  injectStop(seq, now_ms);
  return seq;
}

void BodyLink::injectStop(uint8_t seq, uint32_t now_ms) {
  m_stop_seq = seq;
  m_stop_sends = 0;
  m_stop_last_ms = now_ms;
  sendStopFrame();
}

void BodyLink::sendStopFrame() {
  // STOP is accepted by the Uno from any source in any state, so it goes out even before sync.
  // A full urgent slot (previous STOP still being written) also counts as an attempt.
  emit(proto::StopMsg{static_cast<uint8_t>(proto::Source::kHuman)}, m_stop_seq, true);
  ++m_stop_sends;
}

void BodyLink::pushEvent(const LinkEvent& ev) {
  if (m_ev_count == kEventSlots) {  // full: drop the oldest
    m_ev_head = (m_ev_head + 1) % kEventSlots;
    --m_ev_count;
  }
  m_events[(m_ev_head + m_ev_count) % kEventSlots] = ev;
  ++m_ev_count;
}

bool BodyLink::popEvent(LinkEvent& ev) {
  if (m_ev_count == 0) return false;
  ev = m_events[m_ev_head];
  m_ev_head = (m_ev_head + 1) % kEventSlots;
  --m_ev_count;
  return true;
}

}  // namespace core
}  // namespace cq
```

Run: `pio test -e native`
Expected: `28 test cases: 28 succeeded` (frame_parser 7, frame_tx 5, body_link 16).

- [ ] **Step 6: UART port and `link_task` (Arduino shell)**

`firmware/esp32/src/net/uart_port.h`:
```cpp
// Serial1 to the Uno: RX GPIO3, TX GPIO40, 250000 8N1 (bridge LLD R2.1). Only link_task uses it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

class UartPort {
 public:
  void begin();
  size_t read(uint8_t* buf, size_t max);
  size_t txFree();
  void write(const uint8_t* data, size_t len);  // caller checked txFree() >= len
};

}  // namespace net
}  // namespace cq
```

`firmware/esp32/src/net/uart_port.cpp`:
```cpp
#include "net/uart_port.h"

#include <Arduino.h>

#include "board_config.h"

namespace cq {
namespace net {

void UartPort::begin() {
  Serial1.setRxBufferSize(board::kUartRxBufSize);
  Serial1.setTxBufferSize(board::kUartTxBufSize);
  Serial1.begin(board::kUnoBaud, SERIAL_8N1, board::kUnoRxPin, board::kUnoTxPin);
}

size_t UartPort::read(uint8_t* buf, size_t max) {
  const int avail = Serial1.available();
  if (avail <= 0) return 0;
  return Serial1.read(buf, static_cast<size_t>(avail) < max ? static_cast<size_t>(avail) : max);
}

size_t UartPort::txFree() { return static_cast<size_t>(Serial1.availableForWrite()); }

void UartPort::write(const uint8_t* data, size_t len) { Serial1.write(data, len); }

}  // namespace net
}  // namespace cq
```

`firmware/esp32/src/net/link_task.h`:
```cpp
// link_task: the only owner of the Uno UART and the body-link core objects (bridge LLD R2.11).
// Runs on core 1 every 2 ms. Other tasks talk to it only through linkPost().
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace net {

struct LinkCommand {
  enum class Kind : uint8_t { kWsOpen, kWsClose, kWsText, kOtaStart };
  Kind kind;
  int fd;
  uint16_t len;
  char text[200];  // one WS JSON message (longer ones are refused by the WS handler)
};

void linkTaskStart();
bool linkPost(const LinkCommand& cmd, bool urgent);  // urgent (STOP) goes to the queue front

}  // namespace net
}  // namespace cq
```

`firmware/esp32/src/net/link_task.cpp`:
```cpp
#include "net/link_task.h"

#include <Arduino.h>

#include "board_config.h"
#include "core/body_link.h"
#include "core/frame_tx.h"
#include "net/log.h"
#include "net/uart_port.h"

namespace cq {
namespace net {
namespace {

QueueHandle_t g_queue = nullptr;
UartPort g_uart;
core::FrameTx g_tx;
core::BodyLink g_link(g_tx);

void drainTx() {
  const uint8_t* p = nullptr;
  size_t len = 0;
  while ((len = g_tx.peek(p)) != 0) {
    if (g_uart.txFree() < len) return;  // next tick (UART drains 25 B/ms); never a partial chunk
    g_uart.write(p, len);
    g_tx.pop();
  }
}

void logEvents() {
  core::LinkEvent ev{};
  while (g_link.popEvent(ev)) {
    if (ev.kind == core::LinkEventKind::kBodyOnline) {
      const core::HelloInfo& h = g_link.hello();
      logf("uno: synced fw %u.%u proto %u commit %08lx", h.fw >> 8, h.fw & 0xFF, h.proto,
           static_cast<unsigned long>(h.commit));
    } else if (ev.kind == core::LinkEventKind::kBodyOffline) {
      if (ev.code == 1) {
        logf("uno: incompatible protocol %d (need %u) - flash Plan B firmware", ev.a,
             proto::kProtocolVersion);
      } else {
        logf("uno: body offline (silent)");
      }
    } else if (ev.kind == core::LinkEventKind::kResetRequest) {
      logf("uno: %s reset request ignored (bridge has no stored settings)",
           ev.code == static_cast<uint8_t>(proto::FrameType::kWifiResetReq) ? "WiFi" : "admin");
    }
  }
}

void linkTask(void*) {
  uint8_t rx[256];
  bool hello_logged = false;
#ifdef CQ_MASTER_AT_BOOT
  g_link.startMaster(millis());  // Task 2 hardware check only
#endif
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    size_t n = 0;
    while ((n = g_uart.read(rx, sizeof(rx))) != 0) g_link.onUartBytes(rx, n, now);
    if (!hello_logged && g_link.hello().seen) {
      hello_logged = true;
      const core::HelloInfo& h = g_link.hello();
      logf("uno: HELLO fw %u.%u proto %u caps 0x%02x commit %08lx", h.fw >> 8, h.fw & 0xFF,
           h.proto, h.caps, static_cast<unsigned long>(h.commit));
    }
    LinkCommand cmd;
    while (xQueueReceive(g_queue, &cmd, 0) == pdTRUE) {
      (void)cmd;  // clients arrive in Tasks 4 and 6
    }
    g_link.step(now);
    drainTx();
    logEvents();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(board::kLinkTickMs));
  }
}

}  // namespace

void linkTaskStart() {
  g_uart.begin();
  g_queue = xQueueCreate(16, sizeof(LinkCommand));
  xTaskCreatePinnedToCore(linkTask, "link", 6144, nullptr, 5, nullptr, 1);
  logf("uno link: Serial1 RX %d TX %d %lu baud", board::kUnoRxPin, board::kUnoTxPin,
       static_cast<unsigned long>(board::kUnoBaud));
}

bool linkPost(const LinkCommand& cmd, bool urgent) {
  if (g_queue == nullptr) return false;
  return (urgent ? xQueueSendToFront(g_queue, &cmd, 0) : xQueueSendToBack(g_queue, &cmd, 0)) ==
         pdTRUE;
}

}  // namespace net
}  // namespace cq
```

In `firmware/esp32/src/main.cpp` add `#include "net/link_task.h"` after `#include "net/log.h"`, and at the end of `setup()`:
```cpp
  cq::net::linkTaskStart();
```

Run: `pio run -e bridge`
Expected: `SUCCESS`.

- [ ] **Step 7 [HW]: Passive HELLO, then a master handshake with the Uno (Plan B flashed, tank on a stand)**

Prepare: Plan B firmware on the Uno (protocol 2), 4-pin ESP32↔Uno cable connected, shield switch **S1 in the "ESP32 connected" position**, battery on, tracks off the ground.

Run: `pio run -e bridge -t upload --upload-port /dev/ttyACM0 && pio device monitor -p /dev/ttyACM0 -b 115200`, then press the Uno's reset button.
Expected (the bridge is not master, so it only listens; the Uno repeats HELLO every 500 ms until someone answers):
```
[ …] uno link: Serial1 RX 3 TX 40 250000 baud
[ …] uno: HELLO fw 1.0 proto 2 caps 0x00 commit <8 hex of the Uno build>
```
Then the master check (temporary build flag, nothing is driven):
```bash
PLATFORMIO_BUILD_FLAGS="-D CQ_MASTER_AT_BOOT" pio run -e bridge -t upload --upload-port /dev/ttyACM0 && pio device monitor -p /dev/ttyACM0 -b 115200
```
Expected: `uno: synced fw 1.0 proto 2 commit <8 hex>` within ~1 s of boot, no `body offline` lines afterwards; the Uno's LED shows the "network, no owner" colour (Plan B). Unplug the ESP32↔Uno cable for 2 s → `uno: body offline (silent)`; plug back → `uno: synced …` again. Finally flash the normal build again: `pio run -e bridge -t upload --upload-port /dev/ttyACM0`.

- [ ] **Step 8: Commit**

```bash
git add firmware/esp32/platformio.ini firmware/esp32/scripts/native_paths.py firmware/esp32/src firmware/esp32/test
git commit -m "feat(esp32): body link master with resyncing parser and frame-aligned TX queue

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```
