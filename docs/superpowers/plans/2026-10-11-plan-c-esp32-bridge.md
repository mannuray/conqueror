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
