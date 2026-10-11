# Plan C — ESP32 WiFi Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A small developer firmware for the ELEGOO ESP32-WROVER camera board that joins the home WiFi and transparently pipes bytes between a TCP client on port 3333 and the Uno's UART (250000 baud), plus an optional single-viewer MJPEG stream — so the bench tool can drive the tank wirelessly.

**Architecture:** One PlatformIO environment `bridge` in `firmware/esp32/`. Tiny layered units: `WifiLink` (station → fallback-AP state machine), `MdnsAdvertiser`, `UartPort` (Serial2 on IO33/IO4), `TcpBridge` (single client, fixed buffers, both-way pump, stats), `CameraStream` (optional :81 `/stream`), `StatusIndicator` (USB-serial log, optional LED). The bridge never parses or creates frames; protocol and safety stay end-to-end between laptop and Uno.

**Tech Stack:** PlatformIO (installed with `uv tool`), **pioarduino** platform release **55.03.311** (Arduino-ESP32 **3.3.11**, ESP-IDF **5.5.5**), C++17, esp32-camera (bundled with the core), ESPmDNS, `esp_http_server`; Python 3.12 + Plan A's `conqueror_protocol` for the smoke script.

**Spec:** `docs/lld/esp32-bridge-lld.md` (+ `docs/lld/esp32-firmware-lld.md` §10.1 toolchain, `docs/implementation-roadmap.md` phase 3, `docs/standards/coding-standards.md` §1/§3).

**Roadmap position:** Phase 3, alongside Plan D (bench tool). Needs Plan A only for the smoke script; full end-to-end checks need Plan B firmware on the Uno.

## Global Constraints

- **Platform:** commands are shown for macOS; on **Linux** use the equivalents in `docs/dev-setup.md` (uv install, serial port names `/dev/ttyACM0`/`/dev/ttyUSB0` + `dialout` group, PlatformIO udev rules, `avahi-browse` instead of `dns-sd`, hold-to-drive needs X11).
- Board: ELEGOO ESP32-WROVER camera **V1.0** (no IO13 LED) — status goes to the USB serial log; LED support only with `-D CQ_STATUS_LED` on a V1.2 board.
- Uno link: **UART2, RX = IO33, TX = IO4, 250000 baud, 8N1**.
- Camera: OV3660 on the ELEGOO pin map `CAMERA_MODEL_M5STACK_WIDE`; stream VGA (640×480) JPEG quality 12, **one viewer**, port **81**, path `/stream`; compiled only with `-D CQ_BRIDGE_CAMERA` (on by default in `env:bridge`).
- WiFi: station from `include/secrets.h` (git-ignored), **30 s** timeout → WPA2 fallback AP `conqueror-bridge`; in fallback, retry home WiFi every **60 s** only while **no TCP client** is connected.
- mDNS host `conqueror-bridge.local`; service `_cqbridge._tcp` port 3333 (see deviation below).
- TCP: port **3333**, `TCP_NODELAY`, one client; a new connection **replaces** the old one (logged).
- The bridge **never generates heartbeats or commands**; Uno bytes with no client are **discarded** (counted as drops).
- No heap allocation in the pump loop (fixed 512-byte buffers).
- Boot log prints the IP and the warning `DEVELOPER BRIDGE - NO AUTHENTICATION - home network only`; stats line every **10 s**.
- USB serial log at **115200**.
- Not part of release firmware; the phase-4 `gateway` env is **not** implemented here.
- **Deviation from the LLD (recorded in Task 5):** mDNS service type `_conqueror-bridge._tcp` is 16 characters, over the 15-character DNS-SD service-name limit (RFC 6763 §7.2) — use `_cqbridge._tcp`.

## Review Focus

1. **Laptop vanishes mid-session without closing the socket** (WiFi off, lid closed) → the dead client must not block a new connection; the Uno halts on its own because heartbeats stop (Task 2 check "stale client"; Task 5 safety check).
2. **A second client connects while one is active** → old client closed and logged, new one works (Task 2 check "client replacement").
3. **Home WiFi unreachable or wrong password** → fallback hotspot within 30 s and the bridge reachable through it (Task 3 check "fallback AP").
4. **Bursts larger than the buffers** (10 KB written at once) → delivered intact in order, no loss (Task 2 check "loopback burst").
5. **Uno keeps talking while no client is connected** → bytes discarded, drop counter rises, heap stays stable, no crash (Task 2 check "no-client drops").

---

## File Structure

```
firmware/esp32/
├── platformio.ini                 env:bridge (Task 1)
├── include/
│   ├── board_config.h             pins, ports, sizes (Task 1)
│   ├── secrets.example.h          committed template (Task 1)
│   └── secrets.h                  git-ignored, local only
├── src/
│   ├── bridge_main.cpp            setup()/loop() composition root (Tasks 1–4)
│   └── bridge/
│       ├── status_indicator.h/.cpp   log + optional LED (Task 1)
│       ├── uart_port.h/.cpp          Serial2 wrapper (Task 2)
│       ├── tcp_bridge.h/.cpp         single-client pump + stats (Task 2)
│       ├── wifi_link.h/.cpp          station / fallback AP state machine (Task 3)
│       ├── mdns_advertiser.h/.cpp    host + service (Task 3)
│       └── camera_stream.h/.cpp      optional MJPEG :81 (Task 4)
└── README-bridge.md               usage + security warning (Task 5)
tools/smoke/pyproject.toml         uv project shared with Plan B (created by whichever plan runs first)
tools/smoke/bridge_echo.py         TCP smoke test: loopback + protocol handshake (Tasks 2, 5)
```

---

### Task 1: PlatformIO project, board config, secrets, status log

**Files:**
- Create: `firmware/esp32/platformio.ini`, `firmware/esp32/include/board_config.h`, `firmware/esp32/include/secrets.example.h`, `firmware/esp32/src/bridge/status_indicator.h`, `firmware/esp32/src/bridge/status_indicator.cpp`, `firmware/esp32/src/bridge_main.cpp`
- Modify: `.gitignore` (add `firmware/esp32/include/secrets.h`)

**Interfaces:**
- Produces: `board_config.h` constants `kUnoRxPin = 33`, `kUnoTxPin = 4`, `kUnoBaud = 250000`, `kLogBaud = 115200`, `kTcpPort = 3333`, `kStreamPort = 81`, `kPumpBufSize = 512`, `kStationTimeoutMs = 30000`, `kHomeRetryMs = 60000`, `kStatsPeriodMs = 10000`, `kHostname = "conqueror-bridge"`, `kFallbackSsid = "conqueror-bridge"`, `kStatusLedPin = 13`; `secrets.h` macros `CQ_WIFI_SSID`, `CQ_WIFI_PASSWORD`, `CQ_AP_PASSWORD`; `cq::bridge::StatusIndicator` with `begin()`, `set(LinkState)`, `log(const char* fmt, ...)`; `enum class LinkState : uint8_t { kConnecting, kStation, kFallbackAp, kClientActive }`.

- [ ] **Step 1: Install PlatformIO**

Run: `command -v uv || brew install uv; uv tool install platformio && pio --version`
Expected: `PlatformIO Core, version 6.x.y`.

- [ ] **Step 2: Create the project files**

`firmware/esp32/platformio.ini`:
```ini
; Conqueror ESP32 firmware. env:bridge = developer WiFi<->UART bridge (no auth).
; env:gateway (full firmware) arrives in roadmap phase 4.

[env]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip
framework = arduino
board = esp-wrover-kit
monitor_speed = 115200
upload_speed = 921600
build_unflags = -std=gnu++11
build_flags =
    -std=gnu++17
    -Wall
    -Wextra
    -DBOARD_HAS_PSRAM
    -mfix-esp32-psram-cache-issue
    -DCORE_DEBUG_LEVEL=1

[env:bridge]
build_src_filter = +<bridge_main.cpp> +<bridge/>
build_flags =
    ${env.build_flags}
    -DCQ_BRIDGE_CAMERA
```

`firmware/esp32/include/board_config.h`:
```cpp
// Pins, ports and sizes for the ELEGOO ESP32-WROVER camera board (V1.0 / V1.2).
#pragma once

#include <stdint.h>

namespace cq {
namespace board {

constexpr int kUnoRxPin = 33;  // P1 RX  <- Uno TX
constexpr int kUnoTxPin = 4;   // P1 TX  -> Uno RX
constexpr uint32_t kUnoBaud = 250000;
constexpr uint32_t kLogBaud = 115200;

constexpr uint16_t kTcpPort = 3333;
constexpr uint16_t kStreamPort = 81;
constexpr uint16_t kPumpBufSize = 512;
constexpr uint16_t kUartRxBufSize = 1024;
constexpr uint16_t kUartTxBufSize = 1024;

constexpr uint32_t kStationTimeoutMs = 30000;
constexpr uint32_t kHomeRetryMs = 60000;
constexpr uint32_t kStatsPeriodMs = 10000;

constexpr const char* kHostname = "conqueror-bridge";
constexpr const char* kFallbackSsid = "conqueror-bridge";
constexpr const char* kMdnsService = "cqbridge";  // DNS-SD label <= 15 chars

constexpr int kStatusLedPin = 13;  // only fitted on board V1.2

}  // namespace board
}  // namespace cq
```

`firmware/esp32/include/secrets.example.h`:
```cpp
// Copy to secrets.h (git-ignored) and fill in. Never commit secrets.h.
#pragma once

#define CQ_WIFI_SSID "your-home-ssid"
#define CQ_WIFI_PASSWORD "your-home-password"
#define CQ_AP_PASSWORD "change-me-8-chars-min"
```

Append to `.gitignore`:
```
firmware/esp32/include/secrets.h
```

Run: `cp firmware/esp32/include/secrets.example.h firmware/esp32/include/secrets.h` and edit `secrets.h` with your real home WiFi and an AP password of at least 8 characters.

`firmware/esp32/src/bridge/status_indicator.h`:
```cpp
// Bridge status: always logged on USB serial; LED on IO13 only with CQ_STATUS_LED (board V1.2).
#pragma once

#include <stdint.h>

namespace cq {
namespace bridge {

enum class LinkState : uint8_t { kConnecting, kStation, kFallbackAp, kClientActive };

class StatusIndicator {
 public:
  void begin();
  void set(LinkState state);
  void tick(uint32_t now_ms);  // drives LED blink patterns
  void log(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

 private:
  LinkState m_state = LinkState::kConnecting;
  uint32_t m_last_toggle_ms = 0;
  bool m_led_on = false;
};

const char* toString(LinkState state);

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge/status_indicator.cpp`:
```cpp
#include "bridge/status_indicator.h"

#include <Arduino.h>
#include <stdarg.h>

#include "board_config.h"

namespace cq {
namespace bridge {

const char* toString(LinkState state) {
  switch (state) {
    case LinkState::kConnecting: return "connecting";
    case LinkState::kStation: return "station";
    case LinkState::kFallbackAp: return "fallback-ap";
    case LinkState::kClientActive: return "client-active";
  }
  return "?";
}

void StatusIndicator::begin() {
#ifdef CQ_STATUS_LED
  pinMode(board::kStatusLedPin, OUTPUT);
  digitalWrite(board::kStatusLedPin, LOW);
#endif
}

void StatusIndicator::set(LinkState state) {
  if (state == m_state) {
    return;
  }
  m_state = state;
  log("status: %s", toString(state));
}

void StatusIndicator::tick(uint32_t now_ms) {
#ifdef CQ_STATUS_LED
  uint32_t period_ms = 0;  // 0 = solid on
  switch (m_state) {
    case LinkState::kConnecting: period_ms = 150; break;
    case LinkState::kStation: period_ms = 0; break;
    case LinkState::kFallbackAp: period_ms = 1000; break;
    case LinkState::kClientActive: period_ms = 60; break;
  }
  if (period_ms == 0) {
    digitalWrite(board::kStatusLedPin, HIGH);
    return;
  }
  if (now_ms - m_last_toggle_ms >= period_ms) {
    m_last_toggle_ms = now_ms;
    m_led_on = !m_led_on;
    digitalWrite(board::kStatusLedPin, m_led_on ? HIGH : LOW);
  }
#else
  (void)now_ms;
#endif
}

void StatusIndicator::log(const char* fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof line, fmt, args);
  va_end(args);
  Serial.printf("[bridge %8lu] %s\n", static_cast<unsigned long>(millis()), line);
}

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge_main.cpp` (skeleton, grows in Tasks 2–4):
```cpp
// Composition root of the developer WiFi<->UART bridge. No authentication: home network only.
#include <Arduino.h>

#include "board_config.h"
#include "bridge/status_indicator.h"

using namespace cq;

namespace {
bridge::StatusIndicator g_status;
}  // namespace

void setup() {
  Serial.begin(board::kLogBaud);
  delay(200);
  g_status.begin();
  g_status.log("Conqueror bridge starting");
  g_status.log("DEVELOPER BRIDGE - NO AUTHENTICATION - home network only");
}

void loop() {
  g_status.tick(millis());
  delay(1);
}
```

- [ ] **Step 3: Build**

Run: `cd firmware/esp32 && pio run -e bridge`
Expected: first run downloads the platform/toolchain, then `[SUCCESS]`.

- [ ] **Step 4: Flash and watch the log**

Run: connect the ESP32 board by USB-C, then `cd firmware/esp32 && pio run -e bridge -t upload && pio device monitor -b 115200`
Expected log lines:
```
[bridge      2xx] Conqueror bridge starting
[bridge      2xx] DEVELOPER BRIDGE - NO AUTHENTICATION - home network only
```
Exit the monitor with `Ctrl-C`.

- [ ] **Step 5: Commit**

```bash
git add .gitignore firmware/esp32/platformio.ini firmware/esp32/include/board_config.h firmware/esp32/include/secrets.example.h firmware/esp32/src
git commit -m "feat(esp32-bridge): PlatformIO project (pioarduino 55.03.311), board config, status log"
```

---

### Task 2: UART port and single-client TCP pump with stats

**Files:**
- Create: `firmware/esp32/src/bridge/uart_port.h/.cpp`, `firmware/esp32/src/bridge/tcp_bridge.h/.cpp`, `tools/smoke/bridge_echo.py`; `tools/smoke/pyproject.toml` only if it does not exist yet (shared with Plan B)
- Modify: `firmware/esp32/src/bridge_main.cpp`

**Interfaces:**
- Consumes: `board_config.h`, `StatusIndicator`.
- Produces:
  - `cq::bridge::UartPort` — `begin()`, `size_t read(uint8_t* buf, size_t max)`, `size_t write(const uint8_t* buf, size_t len)`, `size_t available()`.
  - `cq::bridge::BridgeStats { uint32_t bytes_up; uint32_t bytes_down; uint32_t clients; uint32_t drops; uint32_t replaced; }` (up = laptop→Uno, down = Uno→laptop).
  - `cq::bridge::TcpBridge(UartPort&, StatusIndicator&)` — `begin()`, `poll()` (accept/replace), `pump()` (both directions), `bool hasClient()`, `const BridgeStats& stats()`.
  - `tools/smoke/bridge_echo.py --host H [--port 3333] --loopback --bytes N` (raw loopback check).

- [ ] **Step 1: Implement the UART port**

`firmware/esp32/src/bridge/uart_port.h`:
```cpp
// Serial2 to the Uno: RX IO33, TX IO4, 250000 8N1. Only TcpBridge uses it.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cq {
namespace bridge {

class UartPort {
 public:
  void begin();
  size_t available();
  size_t read(uint8_t* buf, size_t max);
  size_t write(const uint8_t* buf, size_t len);
};

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge/uart_port.cpp`:
```cpp
#include "bridge/uart_port.h"

#include <Arduino.h>

#include "board_config.h"

namespace cq {
namespace bridge {

void UartPort::begin() {
  Serial2.setRxBufferSize(board::kUartRxBufSize);
  Serial2.setTxBufferSize(board::kUartTxBufSize);
  Serial2.begin(board::kUnoBaud, SERIAL_8N1, board::kUnoRxPin, board::kUnoTxPin);
}

size_t UartPort::available() { return static_cast<size_t>(Serial2.available()); }

size_t UartPort::read(uint8_t* buf, size_t max) { return Serial2.read(buf, max); }

size_t UartPort::write(const uint8_t* buf, size_t len) { return Serial2.write(buf, len); }

}  // namespace bridge
}  // namespace cq
```

- [ ] **Step 2: Implement the TCP bridge**

`firmware/esp32/src/bridge/tcp_bridge.h`:
```cpp
// Single-client transparent pump: TCP:3333 <-> UART. Never parses or creates frames.
#pragma once

#include <WiFi.h>
#include <stdint.h>

#include "board_config.h"
#include "bridge/status_indicator.h"
#include "bridge/uart_port.h"

namespace cq {
namespace bridge {

struct BridgeStats {
  uint32_t bytes_up = 0;    // laptop -> Uno
  uint32_t bytes_down = 0;  // Uno -> laptop
  uint32_t clients = 0;     // connections accepted
  uint32_t drops = 0;       // Uno bytes discarded (no client / short write)
  uint32_t replaced = 0;    // clients replaced by a newer connection
};

class TcpBridge {
 public:
  TcpBridge(UartPort& uart, StatusIndicator& status);
  void begin();
  void poll();  // accept / replace client, detect disconnects
  void pump();  // move bytes both ways
  bool hasClient();
  const BridgeStats& stats() const { return m_stats; }

 private:
  void pumpTcpToUart();
  void pumpUartToTcp();

  UartPort& m_uart;
  StatusIndicator& m_status;
  WiFiServer m_server{board::kTcpPort};
  WiFiClient m_client;
  bool m_had_client = false;
  BridgeStats m_stats;
  uint8_t m_buf_up[board::kPumpBufSize];
  uint8_t m_buf_down[board::kPumpBufSize];
};

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge/tcp_bridge.cpp`:
```cpp
#include "bridge/tcp_bridge.h"

namespace cq {
namespace bridge {

TcpBridge::TcpBridge(UartPort& uart, StatusIndicator& status) : m_uart(uart), m_status(status) {}

void TcpBridge::begin() {
  m_server.begin();
  m_server.setNoDelay(true);
  m_status.log("tcp: listening on port %u", static_cast<unsigned>(board::kTcpPort));
}

bool TcpBridge::hasClient() { return m_client && m_client.connected(); }

void TcpBridge::poll() {
  if (m_server.hasClient()) {
    WiFiClient incoming = m_server.accept();
    if (hasClient()) {
      m_status.log("tcp: replacing client %s with %s", m_client.remoteIP().toString().c_str(),
                    incoming.remoteIP().toString().c_str());
      m_client.stop();
      ++m_stats.replaced;
    } else {
      m_status.log("tcp: client connected %s", incoming.remoteIP().toString().c_str());
    }
    m_client = incoming;
    m_client.setNoDelay(true);
    ++m_stats.clients;
    m_had_client = true;
  }
  if (m_had_client && !hasClient()) {
    m_status.log("tcp: client disconnected");
    m_client.stop();
    m_had_client = false;
  }
}

void TcpBridge::pumpTcpToUart() {
  if (!hasClient()) {
    return;
  }
  // Drain everything available; bounded per pass so the other direction keeps flowing.
  for (int pass = 0; pass < 8; ++pass) {
    const int avail = m_client.available();
    if (avail <= 0) {
      return;
    }
    const size_t want = static_cast<size_t>(avail) < sizeof m_buf_up ? static_cast<size_t>(avail)
                                                                     : sizeof m_buf_up;
    const int got = m_client.read(m_buf_up, want);
    if (got <= 0) {
      return;
    }
    m_uart.write(m_buf_up, static_cast<size_t>(got));
    m_stats.bytes_up += static_cast<uint32_t>(got);
  }
}

void TcpBridge::pumpUartToTcp() {
  for (int pass = 0; pass < 8; ++pass) {
    const size_t avail = m_uart.available();
    if (avail == 0) {
      return;
    }
    const size_t got = m_uart.read(m_buf_down, avail < sizeof m_buf_down ? avail : sizeof m_buf_down);
    if (got == 0) {
      return;
    }
    if (!hasClient()) {
      m_stats.drops += static_cast<uint32_t>(got);  // nobody listening: discard
      continue;
    }
    const size_t sent = m_client.write(m_buf_down, got);
    m_stats.bytes_down += static_cast<uint32_t>(sent);
    if (sent < got) {
      m_stats.drops += static_cast<uint32_t>(got - sent);
    }
  }
}

void TcpBridge::pump() {
  pumpTcpToUart();
  pumpUartToTcp();
}

}  // namespace bridge
}  // namespace cq
```

- [ ] **Step 3: Wire into main with a temporary soft AP for testing**

Replace `firmware/esp32/src/bridge_main.cpp` with:
```cpp
// Composition root of the developer WiFi<->UART bridge. No authentication: home network only.
#include <Arduino.h>
#include <WiFi.h>

#include "board_config.h"
#include "bridge/status_indicator.h"
#include "bridge/tcp_bridge.h"
#include "bridge/uart_port.h"
#include "secrets.h"

using namespace cq;

namespace {
bridge::StatusIndicator g_status;
bridge::UartPort g_uart;
bridge::TcpBridge g_tcp(g_uart, g_status);
uint32_t g_last_stats_ms = 0;

void logStats(uint32_t now_ms) {
  if (now_ms - g_last_stats_ms < board::kStatsPeriodMs) {
    return;
  }
  g_last_stats_ms = now_ms;
  const bridge::BridgeStats& s = g_tcp.stats();
  g_status.log("stats: up=%lu down=%lu clients=%lu replaced=%lu drops=%lu heap=%lu",
               static_cast<unsigned long>(s.bytes_up), static_cast<unsigned long>(s.bytes_down),
               static_cast<unsigned long>(s.clients), static_cast<unsigned long>(s.replaced),
               static_cast<unsigned long>(s.drops), static_cast<unsigned long>(ESP.getFreeHeap()));
}
}  // namespace

void setup() {
  Serial.begin(board::kLogBaud);
  delay(200);
  g_status.begin();
  g_status.log("Conqueror bridge starting");
  g_status.log("DEVELOPER BRIDGE - NO AUTHENTICATION - home network only");
  g_uart.begin();
  // Temporary until Task 3: plain soft AP so the pump can be tested.
  WiFi.mode(WIFI_AP);
  WiFi.softAP(board::kFallbackSsid, CQ_AP_PASSWORD);
  g_status.log("wifi: temporary AP %s ip %s", board::kFallbackSsid,
               WiFi.softAPIP().toString().c_str());
  g_tcp.begin();
}

void loop() {
  const uint32_t now = millis();
  g_tcp.poll();
  g_tcp.pump();
  g_status.set(g_tcp.hasClient() ? bridge::LinkState::kClientActive
                                 : bridge::LinkState::kFallbackAp);
  g_status.tick(now);
  logStats(now);
  delay(1);
}
```

- [ ] **Step 4: Write the loopback smoke script**

`tools/smoke/` is one uv project shared with Plan B's `uno_smoke.py`. If `tools/smoke/pyproject.toml` already exists (Plan B Task 2 ran first), leave it unchanged; otherwise create it with exactly this content (identical to Plan B Task 2 Step 8):
```toml
[project]
name = "conqueror-smoke"
version = "0.1.0"
description = "Stand and bridge smoke scripts (Plans B and C)"
requires-python = ">=3.12"
dependencies = ["conqueror-protocol", "pyserial>=3.5"]

[tool.uv]
package = false

[tool.uv.sources]
conqueror-protocol = { path = "../../protocol/python", editable = true }

[dependency-groups]
dev = ["ruff>=0.7", "pyright>=1.1", "types-pyserial>=3.5"]

[tool.ruff]
line-length = 100
target-version = "py312"

[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "SIM", "RUF"]

[tool.pyright]
pythonVersion = "3.12"
typeCheckingMode = "strict"
reportMissingTypeStubs = false
```

`tools/smoke/bridge_echo.py`:
```python
"""Bridge smoke test.

--loopback : with a jumper between the ESP32's P1 RX (IO33) and TX (IO4) and the Uno cable
             unplugged, sends random bytes and checks they come back intact.
--handshake: with Plan B firmware on the Uno, sends HEARTBEATs, answers HELLO with HELLO_ACK
             and prints decoded Uno frames.
Run inside the tools/smoke uv project:  cd tools/smoke && uv run python bridge_echo.py ...
"""

from __future__ import annotations

import argparse
import os
import socket
import sys
import time


def loopback(host: str, port: int, total: int) -> int:
    data = os.urandom(total)
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(data)
        received = bytearray()
        deadline = time.monotonic() + 10
        while len(received) < total and time.monotonic() < deadline:
            chunk = sock.recv(4096)
            if not chunk:
                break
            received += chunk
    if bytes(received) == data:
        print(f"loopback OK: {total} bytes echoed intact")
        return 0
    print(f"loopback FAIL: sent {total}, got {len(received)}, equal={bytes(received) == data}")
    return 1


def handshake(host: str, port: int, seconds: float) -> int:
    from conqueror_protocol import FrameParser, build_frame, decode_frame, generated

    parser = FrameParser()
    seq = 0
    synced = False
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(0.02)
        start = time.monotonic()
        next_hb = start
        while time.monotonic() - start < seconds:
            now = time.monotonic()
            if now >= next_hb:
                hb = generated.Heartbeat(esp_ms=int((now - start) * 1000) & 0xFFFFFFFF)
                sock.sendall(build_frame(hb, seq))
                seq = (seq + 1) & 0xFF
                next_hb += 0.1
            try:
                chunk = sock.recv(4096)
            except TimeoutError:
                continue
            if not chunk:
                print("connection closed")
                return 1
            for raw in parser.feed(chunk):
                msg = decode_frame(raw, "uno_to_esp32")
                if isinstance(msg, generated.Hello) and not synced:
                    sock.sendall(
                        build_frame(generated.HelloAck(proto=generated.PROTOCOL_VERSION), seq)
                    )
                    seq = (seq + 1) & 0xFF
                    synced = True
                    print(f"HELLO fw=0x{msg.fw:04X} proto={msg.proto} -> HELLO_ACK sent")
                elif isinstance(msg, generated.TelemFast):
                    print(
                        f"TELEM_FAST yaw={msg.yaw / 100:.2f} dist_mm={msg.dist_mm} "
                        f"flags=0x{msg.flags:04X}"
                    )
                else:
                    print(type(msg).__name__, msg)
    print(f"done: frames={parser.stats.frames} bad_crc={parser.stats.bad_crc} synced={synced}")
    return 0 if synced else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="conqueror-bridge.local")
    ap.add_argument("--port", type=int, default=3333)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--loopback", action="store_true")
    mode.add_argument("--handshake", action="store_true")
    ap.add_argument("--bytes", type=int, default=10240)
    ap.add_argument("--seconds", type=float, default=10.0)
    args = ap.parse_args()
    if args.loopback:
        return loopback(args.host, args.port, args.bytes)
    return handshake(args.host, args.port, args.seconds)


if __name__ == "__main__":
    sys.exit(main())
```

Run (from `tools/smoke/`): `uv sync && uv run ruff format --check . && uv run ruff check . && uv run pyright`
Expected: `… already formatted`, `All checks passed!`, `0 errors, 0 warnings, 0 informations` (strict mode, same gate as Plan B's `uno_smoke.py`; the check covers every script in `tools/smoke/`).

- [ ] **Step 5: Build and flash**

Run: `cd firmware/esp32 && pio run -e bridge -t upload && pio device monitor -b 115200`
Expected: log shows `wifi: temporary AP conqueror-bridge ip 192.168.4.1` and `tcp: listening on port 3333`; a `stats:` line every 10 s.

- [ ] **Step 6: Manual checks (laptop joined to the `conqueror-bridge` AP)**

Prepare: unplug the 4-pin cable to the Uno; put a jumper wire between P1 pin RX (IO33) and TX (IO4).

1. **Loopback burst** (Review Focus 4): `cd tools/smoke && uv run python bridge_echo.py --host 192.168.4.1 --loopback --bytes 10240`
   Expected: `loopback OK: 10240 bytes echoed intact`; log `tcp: client connected 192.168.4.x`, then `tcp: client disconnected`.
2. **Interactive**: `nc 192.168.4.1 3333`, type `hello` + Enter. Expected: `hello` echoed back. Leave it open.
3. **Client replacement** (Review Focus 2): in a second terminal `nc 192.168.4.1 3333`. Expected log `tcp: replacing client …`; the first `nc` exits; typing in the second echoes.
4. **Stale client** (Review Focus 1): `nc 192.168.4.1 3333`, then turn laptop WiFi off for 10 s and on again, reconnect to the AP, run `nc 192.168.4.1 3333` again. Expected: the new connection works (replacement logged if the old socket still looked alive).
5. **No-client drops** (Review Focus 5): remove the jumper, reconnect the Uno cable with any firmware that prints (e.g. the original ELEGOO firmware), no client connected for 30 s. Expected: `stats:` lines show `drops` increasing, `heap` steady (±1 KB), no reboot.

- [ ] **Step 7: Commit**

```bash
git add firmware/esp32/src tools/smoke/bridge_echo.py tools/smoke/pyproject.toml
git commit -m "feat(esp32-bridge): UART port, single-client TCP pump with stats, loopback smoke script"
```

---

### Task 3: WiFi station with fallback AP, mDNS

**Files:**
- Create: `firmware/esp32/src/bridge/wifi_link.h/.cpp`, `firmware/esp32/src/bridge/mdns_advertiser.h/.cpp`
- Modify: `firmware/esp32/src/bridge_main.cpp`

**Interfaces:**
- Consumes: `secrets.h`, `board_config.h`, `StatusIndicator`, `TcpBridge::hasClient()`.
- Produces:
  - `cq::bridge::WifiState : uint8_t { kConnecting, kStation, kFallbackAp }`.
  - `cq::bridge::WifiLink(StatusIndicator&)` — `begin()`, `void step(uint32_t now_ms, bool client_connected)`, `WifiState state() const`, `IPAddress ip() const`, `bool ipChanged()` (true once after each new IP).
  - `cq::bridge::MdnsAdvertiser` — `bool refresh(IPAddress ip)` (true on success; starts or restarts mDNS host `conqueror-bridge` + service `_cqbridge._tcp:3333`).

- [ ] **Step 1: Implement the WiFi link**

`firmware/esp32/src/bridge/wifi_link.h`:
```cpp
// Station on the home network (secrets.h); after 30 s falls back to a WPA2 AP.
// In fallback, retries the home network every 60 s only while no TCP client is connected.
#pragma once

#include <IPAddress.h>
#include <stdint.h>

#include "bridge/status_indicator.h"

namespace cq {
namespace bridge {

enum class WifiState : uint8_t { kConnecting, kStation, kFallbackAp };

class WifiLink {
 public:
  explicit WifiLink(StatusIndicator& status) : m_status(status) {}
  void begin();
  void step(uint32_t now_ms, bool client_connected);
  WifiState state() const { return m_state; }
  IPAddress ip() const { return m_ip; }
  bool ipChanged();

 private:
  void enterConnecting(uint32_t now_ms);
  void enterStation();
  void enterFallback(uint32_t now_ms);
  void setIp(IPAddress ip);

  StatusIndicator& m_status;
  WifiState m_state = WifiState::kConnecting;
  uint32_t m_since_ms = 0;
  uint32_t m_last_retry_ms = 0;
  bool m_ap_up = false;
  IPAddress m_ip;
  bool m_ip_changed = false;
};

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge/wifi_link.cpp`:
```cpp
#include "bridge/wifi_link.h"

#include <WiFi.h>

#include "board_config.h"
#include "secrets.h"

namespace cq {
namespace bridge {

void WifiLink::begin() {
  WiFi.persistent(false);
  WiFi.setHostname(board::kHostname);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  enterConnecting(millis());
}

bool WifiLink::ipChanged() {
  const bool changed = m_ip_changed;
  m_ip_changed = false;
  return changed;
}

void WifiLink::setIp(IPAddress ip) {
  if (ip != m_ip) {
    m_ip = ip;
    m_ip_changed = true;
  }
}

void WifiLink::enterConnecting(uint32_t now_ms) {
  m_state = WifiState::kConnecting;
  m_since_ms = now_ms;
  WiFi.begin(CQ_WIFI_SSID, CQ_WIFI_PASSWORD);
  m_status.set(LinkState::kConnecting);
  m_status.log("wifi: connecting to %s", CQ_WIFI_SSID);
}

void WifiLink::enterStation() {
  m_state = WifiState::kStation;
  if (m_ap_up) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    m_ap_up = false;
  }
  setIp(WiFi.localIP());
  m_status.set(LinkState::kStation);
  m_status.log("wifi: station ip %s rssi %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

void WifiLink::enterFallback(uint32_t now_ms) {
  m_state = WifiState::kFallbackAp;
  m_last_retry_ms = now_ms;
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(board::kFallbackSsid, CQ_AP_PASSWORD);
  m_ap_up = true;
  setIp(WiFi.softAPIP());
  m_status.set(LinkState::kFallbackAp);
  m_status.log("wifi: fallback AP %s ip %s", board::kFallbackSsid,
               WiFi.softAPIP().toString().c_str());
}

void WifiLink::step(uint32_t now_ms, bool client_connected) {
  const bool sta_up = WiFi.status() == WL_CONNECTED;
  switch (m_state) {
    case WifiState::kConnecting:
      if (sta_up) {
        enterStation();
      } else if (now_ms - m_since_ms >= board::kStationTimeoutMs) {
        enterFallback(now_ms);
      }
      break;
    case WifiState::kStation:
      if (!sta_up) {
        m_status.log("wifi: station link lost");
        enterConnecting(now_ms);
      } else {
        setIp(WiFi.localIP());
      }
      break;
    case WifiState::kFallbackAp:
      if (sta_up) {
        enterStation();
      } else if (!client_connected && now_ms - m_last_retry_ms >= board::kHomeRetryMs) {
        m_last_retry_ms = now_ms;
        m_status.log("wifi: retrying home network %s (AP stays up)", CQ_WIFI_SSID);
        WiFi.begin(CQ_WIFI_SSID, CQ_WIFI_PASSWORD);
      }
      break;
  }
}

}  // namespace bridge
}  // namespace cq
```

- [ ] **Step 2: Implement the mDNS advertiser**

`firmware/esp32/src/bridge/mdns_advertiser.h`:
```cpp
// mDNS host conqueror-bridge.local and service _cqbridge._tcp:3333; restarted on IP change.
#pragma once

#include <IPAddress.h>

namespace cq {
namespace bridge {

class MdnsAdvertiser {
 public:
  bool refresh(IPAddress ip);  // returns true when (re)started successfully

 private:
  bool m_started = false;
};

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge/mdns_advertiser.cpp`:
```cpp
#include "bridge/mdns_advertiser.h"

#include <ESPmDNS.h>

#include "board_config.h"

namespace cq {
namespace bridge {

bool MdnsAdvertiser::refresh(IPAddress ip) {
  (void)ip;
  if (m_started) {
    MDNS.end();
    m_started = false;
  }
  if (!MDNS.begin(board::kHostname)) {
    return false;
  }
  MDNS.addService(board::kMdnsService, "tcp", board::kTcpPort);
  MDNS.addServiceTxt(board::kMdnsService, "tcp", "role", "dev-bridge");
  m_started = true;
  return true;
}

}  // namespace bridge
}  // namespace cq
```

- [ ] **Step 3: Wire WiFi + mDNS into main**

Replace `firmware/esp32/src/bridge_main.cpp` with:
```cpp
// Composition root of the developer WiFi<->UART bridge. No authentication: home network only.
#include <Arduino.h>
#include <WiFi.h>

#include "board_config.h"
#include "bridge/mdns_advertiser.h"
#include "bridge/status_indicator.h"
#include "bridge/tcp_bridge.h"
#include "bridge/uart_port.h"
#include "bridge/wifi_link.h"

using namespace cq;

namespace {
bridge::StatusIndicator g_status;
bridge::UartPort g_uart;
bridge::TcpBridge g_tcp(g_uart, g_status);
bridge::WifiLink g_wifi(g_status);
bridge::MdnsAdvertiser g_mdns;
bool g_tcp_started = false;
uint32_t g_last_stats_ms = 0;

void logStats(uint32_t now_ms) {
  if (now_ms - g_last_stats_ms < board::kStatsPeriodMs) {
    return;
  }
  g_last_stats_ms = now_ms;
  const bridge::BridgeStats& s = g_tcp.stats();
  g_status.log("stats: up=%lu down=%lu clients=%lu replaced=%lu drops=%lu heap=%lu",
               static_cast<unsigned long>(s.bytes_up), static_cast<unsigned long>(s.bytes_down),
               static_cast<unsigned long>(s.clients), static_cast<unsigned long>(s.replaced),
               static_cast<unsigned long>(s.drops), static_cast<unsigned long>(ESP.getFreeHeap()));
}

void onNewIp() {
  const IPAddress ip = g_wifi.ip();
  g_status.log("net: ip %s  ->  connect bench to %s.local:%u (or %s:%u)", ip.toString().c_str(),
               board::kHostname, static_cast<unsigned>(board::kTcpPort), ip.toString().c_str(),
               static_cast<unsigned>(board::kTcpPort));
  if (!g_mdns.refresh(ip)) {
    g_status.log("mdns: failed to start");
  }
  if (!g_tcp_started) {
    g_tcp.begin();
    g_tcp_started = true;
  }
}
}  // namespace

void setup() {
  Serial.begin(board::kLogBaud);
  delay(200);
  g_status.begin();
  g_status.log("Conqueror bridge starting");
  g_status.log("DEVELOPER BRIDGE - NO AUTHENTICATION - home network only");
  g_uart.begin();
  g_wifi.begin();
}

void loop() {
  const uint32_t now = millis();
  g_wifi.step(now, g_tcp_started && g_tcp.hasClient());
  if (g_wifi.ipChanged()) {
    onNewIp();
  }
  if (g_tcp_started) {
    g_tcp.poll();
    g_tcp.pump();
    if (g_tcp.hasClient()) {
      g_status.set(bridge::LinkState::kClientActive);
    } else if (g_wifi.state() == bridge::WifiState::kStation) {
      g_status.set(bridge::LinkState::kStation);
    } else if (g_wifi.state() == bridge::WifiState::kFallbackAp) {
      g_status.set(bridge::LinkState::kFallbackAp);
    }
  }
  g_status.tick(now);
  logStats(now);
  delay(1);
}
```

- [ ] **Step 4: Build and flash**

Run: `cd firmware/esp32 && pio run -e bridge -t upload && pio device monitor -b 115200`
Expected (with correct `secrets.h`):
```
[bridge ...] wifi: connecting to <your-ssid>
[bridge ...] status: station
[bridge ...] wifi: station ip 192.168.x.y rssi -5x
[bridge ...] net: ip 192.168.x.y  ->  connect bench to conqueror-bridge.local:3333 (or 192.168.x.y:3333)
[bridge ...] tcp: listening on port 3333
```

- [ ] **Step 5: Manual checks (laptop on the home WiFi)**

1. `ping -c 3 conqueror-bridge.local` → 3 replies from the logged IP.
2. `dns-sd -B _cqbridge._tcp` (macOS) → lists `conqueror-bridge`; stop with Ctrl-C.
3. Jumper loopback over the home network: `cd tools/smoke && uv run python bridge_echo.py --host conqueror-bridge.local --loopback --bytes 10240` → `loopback OK`.
4. **Fallback AP** (Review Focus 3): change `CQ_WIFI_PASSWORD` in `secrets.h` to a wrong value, re-flash. Expected within ~30 s: `wifi: fallback AP conqueror-bridge ip 192.168.4.1`; join that AP from the laptop; `cd tools/smoke && uv run python bridge_echo.py --host 192.168.4.1 --loopback` → OK; with no client, every 60 s `wifi: retrying home network …`; while a client is connected no retry lines appear. Restore the correct password and re-flash.
5. **Station loss**: switch the home router's WiFi off for 20 s, then on. Expected: `wifi: station link lost` → `connecting` → `station` again (same or new IP; `net: ip …` logged when it changes).

- [ ] **Step 6: Commit**

```bash
git add firmware/esp32/src
git commit -m "feat(esp32-bridge): home WiFi station with fallback AP and mDNS"
```

---

### Task 4: Optional single-viewer MJPEG stream (`CQ_BRIDGE_CAMERA`)

**Files:**
- Create: `firmware/esp32/src/bridge/camera_stream.h/.cpp`
- Modify: `firmware/esp32/src/bridge_main.cpp`

**Interfaces:**
- Consumes: `board_config.h`, `StatusIndicator`.
- Produces: `cq::bridge::CameraStream(StatusIndicator&)` — `bool begin()` (camera init + :81 server; logs sensor PID), only compiled with `CQ_BRIDGE_CAMERA`.

- [ ] **Step 1: Implement the camera stream**

`firmware/esp32/src/bridge/camera_stream.h`:
```cpp
// Minimal single-viewer MJPEG stream on :81/stream (dev only; the gateway gets the full camera service).
#pragma once

#include "bridge/status_indicator.h"

namespace cq {
namespace bridge {

class CameraStream {
 public:
  explicit CameraStream(StatusIndicator& status) : m_status(status) {}
  bool begin();

 private:
  StatusIndicator& m_status;
};

}  // namespace bridge
}  // namespace cq
```

`firmware/esp32/src/bridge/camera_stream.cpp`:
```cpp
#include "bridge/camera_stream.h"

#ifdef CQ_BRIDGE_CAMERA

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_http_server.h>

#include "board_config.h"

namespace cq {
namespace bridge {
namespace {

// ELEGOO ESP32-WROVER camera board = CAMERA_MODEL_M5STACK_WIDE pin map.
constexpr int kPwdn = -1;
constexpr int kReset = 15;
constexpr int kXclk = 27;
constexpr int kSiod = 22;
constexpr int kSioc = 23;
constexpr int kY9 = 19;
constexpr int kY8 = 36;
constexpr int kY7 = 18;
constexpr int kY6 = 39;
constexpr int kY5 = 5;
constexpr int kY4 = 34;
constexpr int kY3 = 35;
constexpr int kY2 = 32;
constexpr int kVsync = 25;
constexpr int kHref = 26;
constexpr int kPclk = 21;

constexpr const char* kBoundary = "cqframe";
volatile bool g_viewer_active = false;

esp_err_t streamHandler(httpd_req_t* req) {
  if (g_viewer_active) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "bridge stream supports one viewer");
  }
  g_viewer_active = true;
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=cqframe");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char header[96];
  esp_err_t res = ESP_OK;
  while (res == ESP_OK) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb == nullptr) {
      res = ESP_FAIL;
      break;
    }
    const int len = snprintf(header, sizeof header,
                             "\r\n--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                             kBoundary, static_cast<unsigned>(fb->len));
    res = httpd_resp_send_chunk(req, header, len);
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, reinterpret_cast<const char*>(fb->buf), fb->len);
    }
    esp_camera_fb_return(fb);
  }
  g_viewer_active = false;
  return res;
}

}  // namespace

bool CameraStream::begin() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = kY2;
  config.pin_d1 = kY3;
  config.pin_d2 = kY4;
  config.pin_d3 = kY5;
  config.pin_d4 = kY6;
  config.pin_d5 = kY7;
  config.pin_d6 = kY8;
  config.pin_d7 = kY9;
  config.pin_xclk = kXclk;
  config.pin_pclk = kPclk;
  config.pin_vsync = kVsync;
  config.pin_href = kHref;
  config.pin_sccb_sda = kSiod;
  config.pin_sccb_scl = kSioc;
  config.pin_pwdn = kPwdn;
  config.pin_reset = kReset;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;
  const esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    m_status.log("camera: init failed 0x%x (stream disabled)", static_cast<unsigned>(err));
    return false;
  }
  sensor_t* sensor = esp_camera_sensor_get();
  m_status.log("camera: sensor PID 0x%04x, VGA q12", sensor != nullptr ? sensor->id.PID : 0);

  httpd_config_t http = HTTPD_DEFAULT_CONFIG();
  http.server_port = board::kStreamPort;
  http.ctrl_port = 32769;
  http.max_open_sockets = 2;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &http) != ESP_OK) {
    m_status.log("camera: stream server failed to start");
    return false;
  }
  httpd_uri_t uri = {};
  uri.uri = "/stream";
  uri.method = HTTP_GET;
  uri.handler = streamHandler;
  httpd_register_uri_handler(server, &uri);
  m_status.log("camera: stream on :%u/stream (one viewer)", static_cast<unsigned>(board::kStreamPort));
  return true;
}

}  // namespace bridge
}  // namespace cq

#endif  // CQ_BRIDGE_CAMERA
```

- [ ] **Step 2: Start the camera from main**

In `firmware/esp32/src/bridge_main.cpp`:
- add `#include "bridge/camera_stream.h"` after the other bridge includes;
- inside the anonymous namespace add:
```cpp
#ifdef CQ_BRIDGE_CAMERA
bridge::CameraStream g_camera(g_status);
bool g_camera_started = false;
#endif
```
- at the end of `onNewIp()` add:
```cpp
#ifdef CQ_BRIDGE_CAMERA
  if (!g_camera_started) {
    g_camera_started = g_camera.begin();
  }
#endif
```

- [ ] **Step 3: Build with and without the camera**

Run: `cd firmware/esp32 && pio run -e bridge && PLATFORMIO_BUILD_FLAGS="-UCQ_BRIDGE_CAMERA" pio run -e bridge`
Expected: both `[SUCCESS]` (the second build compiles `camera_stream.cpp` to an empty unit).

- [ ] **Step 4: Flash and check the stream**

Run: `cd firmware/esp32 && pio run -e bridge -t upload && pio device monitor -b 115200`
Expected log: `camera: sensor PID 0x3660, VGA q12` and `camera: stream on :81/stream (one viewer)`.
1. Open `http://conqueror-bridge.local:81/stream` in a browser → live video.
2. Open the same URL in a second tab → text `bridge stream supports one viewer`.
3. Close the first tab, reload the second → video.
4. While streaming, run `cd tools/smoke && uv run python bridge_echo.py --host conqueror-bridge.local --loopback` (jumper fitted) → `loopback OK` (camera traffic does not corrupt the byte pump).

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/src
git commit -m "feat(esp32-bridge): optional single-viewer MJPEG stream on :81"
```

---

### Task 5: Protocol handshake smoke test, README, end-to-end safety check

**Files:**
- Create: `firmware/esp32/README-bridge.md`
- Modify: `docs/lld/esp32-bridge-lld.md` (record the mDNS service-name deviation)

**Interfaces:**
- Consumes: `tools/smoke/bridge_echo.py --handshake` (Task 2), Plan A `conqueror_protocol`, Plan B Uno firmware (for steps 3–4).

- [ ] **Step 1: Write the README**

`firmware/esp32/README-bridge.md`:
```markdown
# ESP32 developer WiFi bridge (`env:bridge`)

> **DEVELOPER BRIDGE — NO AUTHENTICATION.** Anyone on the same network can drive the tank through
> port 3333. Use only on your home network. Never ship this in release firmware.

Transparent pipe: TCP `conqueror-bridge.local:3333` ⇄ Uno UART (IO33 RX / IO4 TX, 250000 baud).
The bridge never creates frames: if the laptop disappears, heartbeats stop and the Uno halts
within 300 ms.

## Setup
1. `cp include/secrets.example.h include/secrets.h` and fill in home WiFi + an AP password (≥ 8 chars).
2. `pio run -e bridge -t upload && pio device monitor -b 115200` — the log prints the IP.
3. Shield switch S1 must be in the "ESP32 connected" position (flip it only for USB uploads to the Uno).

## Finding it
- `ping conqueror-bridge.local`, or `dns-sd -B _cqbridge._tcp`, or read the IP from the USB log.
- Home WiFi unreachable for 30 s → hotspot `conqueror-bridge` (WPA2, password from `secrets.h`),
  bridge at `192.168.4.1`.

## Smoke tests
- Raw loopback (jumper P1 RX↔TX, Uno cable unplugged):
  `cd tools/smoke && uv run python bridge_echo.py --host conqueror-bridge.local --loopback`
- Protocol handshake (Plan B firmware on the Uno), from the repo root (both commands use the `tools/smoke` uv project):
  `cd tools/smoke && uv run python bridge_echo.py --host conqueror-bridge.local --handshake --seconds 10`

## Camera
`http://conqueror-bridge.local:81/stream` — one viewer (built with `-DCQ_BRIDGE_CAMERA`, default on).

## Notes
- One TCP client at a time; a new connection replaces the old one.
- mDNS service is `_cqbridge._tcp` (the LLD's `_conqueror-bridge._tcp` exceeds the 15-character DNS-SD limit).
- Board V1.0 has no status LED; status is on the USB serial log. On V1.2 add `-DCQ_STATUS_LED`.
```

- [ ] **Step 2: Record the deviation in the LLD**

Append to `docs/lld/esp32-bridge-lld.md`:
```markdown

## Implementation note (Plan C)
- mDNS service type is `_cqbridge._tcp` (DNS-SD service names are limited to 15 characters; `_conqueror-bridge` is 16). Host name stays `conqueror-bridge.local`.
- Toolchain pinned: pioarduino platform 55.03.311 (Arduino-ESP32 3.3.11, ESP-IDF 5.5.5), board `esp-wrover-kit`.
- Camera stream compiled only with `-DCQ_BRIDGE_CAMERA` (default on in `env:bridge`).
```

- [ ] **Step 3: Protocol handshake check (after Plan B firmware is on the Uno)**

Prepare: remove the loopback jumper, reconnect the 4-pin cable to the Uno, shield switch S1 to "ESP32 connected", power the tank.
Run: `cd tools/smoke && uv run python bridge_echo.py --host conqueror-bridge.local --handshake --seconds 10`
Expected:
```
HELLO fw=0x0100 proto=1 -> HELLO_ACK sent
TELEM_FAST yaw=… dist_mm=… flags=0x8…      (bit 15 SYNCED set after the handshake)
…
done: frames=… bad_crc=0 synced=True
```

- [ ] **Step 4: End-to-end safety check (after Plan B; tank on a stand, tracks off the ground)**

1. Connect the bench tool (Plan D) via `--host conqueror-bridge.local`, start a `turn by 360` at 30 %.
2. While it turns, switch the laptop's WiFi off.
   Expected: the tracks stop within ~0.3 s (Uno link-loss rule); after WiFi returns the turn does **not** resume.
3. Reconnect and repeat with `Ctrl-C` on the bench tool instead of WiFi off → same result.

- [ ] **Step 5: Commit**

```bash
git add firmware/esp32/README-bridge.md docs/lld/esp32-bridge-lld.md
git commit -m "docs(esp32-bridge): README with security warning, LLD implementation notes"
```

---

## Done when

- `pio run -e bridge` builds with and without `CQ_BRIDGE_CAMERA`.
- On hardware: station on home WiFi with `conqueror-bridge.local` resolving; fallback AP after 30 s with wrong credentials; 10 KB loopback intact; client replacement logged; no-client drops counted with stable heap; one-viewer stream works.
- After Plan B: handshake smoke test synced with `bad_crc=0`; tank stops within ~0.3 s when the laptop's WiFi drops and does not resume.
