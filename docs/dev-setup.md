# Developer machine setup (macOS and Linux)

The implementation plans show macOS commands. This page gives the Linux equivalents. Use the column for your machine.

## 1. Base tools
| Need | macOS | Linux (Debian/Ubuntu) |
|---|---|---|
| Git + C/C++ compiler | `xcode-select --install` | `sudo apt install -y git build-essential` |
| uv (Python 3.12 is installed by uv automatically) | `brew install uv` | `curl -LsSf https://astral.sh/uv/install.sh \| sh` then open a new shell |
| PlatformIO (Plans B, C) | `uv tool install --python 3.12 platformio` | `uv tool install --python 3.12 platformio` |
| esptool (Plan C, ESP32-S3) | `uv tool install --python 3.12 esptool` | `uv tool install --python 3.12 esptool` |
| Claude Code + superpowers plugin | same on both — install Claude Code, then add the superpowers plugin | same |
| GitHub access | SSH key added to GitHub | same (`ssh-keygen -t ed25519`, add the public key to GitHub) |

> `--python 3.12`: without it uv may reuse the system Python (macOS ships 3.9), which the pioarduino ESP32 platform and our build scripts (`datetime.UTC`) do not support. Re-run with `--force` to switch an existing install.

## 1b. After every fresh clone (macOS and Linux)
Run these once in the repo root; the commands are the same on both:
```sh
git config core.hooksPath .githooks   # enables the protocol staleness hook
cd protocol && uv sync                # uv picks Python 3.12 from protocol/.python-version
```
The pre-commit hook (`.githooks/pre-commit` → `scripts/check-protocol.sh`) runs `uv run protogen check` in `protocol/`. It fails the commit if `protocol/defs/*.yaml` changed without regenerating the outputs; fix with `cd protocol && uv run protogen write`. Without `core.hooksPath` the hook never runs.

## 2. Serial ports (Uno and ESP32 over USB)
| | macOS | Linux |
|---|---|---|
| Port names | `/dev/tty.usbserial-*`, `/dev/tty.usbmodem*`, `/dev/tty.wchusbserial*` | `/dev/ttyACM0` (Uno with 16U2), `/dev/ttyUSB0` (CH340: ESP32 board, Uno clones) |
| Permissions | none | `sudo usermod -aG dialout $USER`, then **log out and back in** |
| PlatformIO udev rules | not needed | `curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \| sudo tee /etc/udev/rules.d/99-platformio-udev.rules && sudo udevadm control --reload-rules && sudo udevadm trigger` |
| List ports | `ls /dev/tty.*` or `pio device list` | `ls /dev/ttyACM* /dev/ttyUSB*` or `pio device list` |
If a plan says `--port /dev/tty.usbserial-XXXX`, use `--port /dev/ttyACM0` (or `/dev/ttyUSB0`) on Linux.

## 3. Finding the WiFi bridge (mDNS)
| | macOS | Linux |
|---|---|---|
| `.local` name resolution | built in | `sudo apt install -y avahi-daemon libnss-mdns` (usually preinstalled on desktop Ubuntu) |
| Browse the bridge service | `dns-sd -B _http._tcp` | `avahi-browse -rt _http._tcp` (package `avahi-utils`) |
| Ping | `ping conqueror-bridge.local` | same |

## 4. Bench tool
Retired 2026-10-18 (design-notes §74); the laptop tools are `tools/smoke/` (`move.py`, `turn.py`, `td_record.py`, `straight_report.py`).

## 5. Firewall
The laptop connects **out** to the bridge on HTTP 80 / 81 and OTA 3232; no inbound ports are needed. If a strict firewall blocks mDNS, allow UDP 5353 on the local network or connect by IP.

## 6. Working on two machines
- One branch per plan (`plan-a`, `plan-b`, …); merge Plan A to `main` before starting B/C/D.
- The machine physically connected to the tank runs the hardware steps of Plans B and C; Plans A and D (and B's host tests) run anywhere.
