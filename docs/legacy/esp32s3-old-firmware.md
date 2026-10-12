# ESP32-S3 firmware dump analysis: pin map, UART link, HTTP/UDP interface

Source: `~/tank-dump/esp32s3-flash.bin` on 192.168.31.8 (8 MB full flash). Read-only analysis.
Extracted working copies were deleted after the analysis; only the original dump remains.
No credentials are reproduced here. The SSID and password are stored at runtime in NVS (Preferences namespace `wifi`, keys `ssid` and `pass`), not in the code, and the AP SSID literal is deliberately left out.

## Image facts
- Partition table: nvs 0x9000, otadata 0xe000, app0 0x10000 (1.25 MB), app1 0x150000, spiffs 0x290000, coredump 0x3f0000.
- otadata sequence = 1, so app0 is the running app. app1 holds no valid image (no 0xE9 magic).
- App header: ESP32-S3, entry 0x40375da0, header flash size 4MB, DIO 80 MHz. Built with arduino-esp32 **3.3.7** (paths in the strings), ESP-IDF v5.5.2-729, and the app descriptor dated Feb 11 2026. Uses the `WebServer` library (portal) and `esp_http_server` (camera).
- Segments: DROM 0x3c0c0020, DRAM 0x3fc97d00 and 0x3fc9b65c, IROM 0x42000020, IRAM 0x40374000. The sketch's literal pool starts at 0x42000020, and setup and the helpers are at 0x420030xx–0x42003dxx.
- Disassembly tool: `xtensa-esp32s3-elf-objdump -D -b binary -m xtensa --adjust-vma=0x42000020`.

## 1. UART link to the Uno (high confidence)
At 0x420034a3 there is `call8 0x4200ce74`, which is HardwareSerial::begin. `this` is 0x3fc9fe14, the Serial1 object.

```
4200348d: l32r a12, (0x800001c)   ; config = SERIAL_8N1
42003490: l32r a11, (0x1c200)     ; baud   = 115200
4200349a: movi a13, 3             ; rxPin  = GPIO 3
42003497: movi a14, 40            ; txPin  = GPIO 40
42003495: movi.n a15, 0           ; invert = false
stack[0] = 0x4e20 (20000)         ; timeout_ms (the default)
stack[4] = 120                    ; rxfifo_full_thrhd (the default)
```
- This reads as **`Serial1.begin(115200, SERIAL_8N1, 3 /*RX*/, 40 /*TX*/)`**. Right after it comes a call with argument 10 on the same object, which is probably `Serial1.setTimeout(10)` (medium confidence).
- Just before that, `Serial.begin(115200)` is called on object 0x3fc9fd5c. It is a single-argument begin, which fits the USB-CDC (HWCDC) console.
- Why I identify 0x3fc9fe14 as Serial1: loop prints `"[DBG] Serial1 avail:%d WiFi:%s"` using `available()` on this object, and the object is separate from the console. I could not confirm the UART number field statically, because the object lives in .bss.
- **This contradicts CLAUDE.md** ("IO33 RX / IO4 TX at 250000 baud"). Those facts came from the old WROVER board. This S3 firmware uses **115200 baud, ESP RX = GPIO3, ESP TX = GPIO40**. GPIO3 is a strapping pin (JTAG source select), but it works as a UART pin.

## 2. Camera config (high confidence; matches ESP32-S3-EYE / "ESP32S3_EYE" exactly)
The function at 0x420031c8 (`entry a1,144`, called from setup at 0x420034b3) fills `camera_config_t` at a1+8 and then calls esp_camera_init (0x42052078). Field offset = stack offset − 8, using the esp32-camera layout:

| Field | Value | Evidence |
|---|---|---|
| pin_pwdn | -1 | s32i a9(-1), a1+8 |
| pin_reset | -1 | a1+12 |
| pin_xclk | 15 | a4=15 → a1+16 |
| pin_sccb_sda | 4 | a1+20 |
| pin_sccb_scl | 5 | a1+24 |
| pin_d7 (Y9) | 16 | a1+28 |
| pin_d6 (Y8) | 17 | a1+32 |
| pin_d5 (Y7) | 18 | a1+36 |
| pin_d4 (Y6) | 12 | a1+40 |
| pin_d3 (Y5) | 10 | a1+44 |
| pin_d2 (Y4) | 8 | a1+48 |
| pin_d1 (Y3) | 9 | a1+52 |
| pin_d0 (Y2) | 11 | a1+56 (`movi.n a8,11` at entry) |
| pin_vsync | 6 | a1+60 |
| pin_href | 7 | a1+64 |
| pin_pclk | 13 | a1+68 |
| xclk_freq_hz | 20 000 000 | l32r 0x1312d00 → a1+72 |
| ledc_timer / channel | 0 / 0 | a1+76, a1+80 |
| pixel_format | 4 = PIXFORMAT_JPEG | a1+84 |
| sccb_i2c_port | not set by the sketch | — |

The sketch branches on `psramFound()` (call 0x4200f230):
- With PSRAM: frame_size 15 (UXGA), jpeg_quality 10, fb_count 2, fb_location 0 (CAMERA_FB_IN_PSRAM), grab_mode 1 (CAMERA_GRAB_LATEST).
- Without PSRAM: frame_size 6 (QVGA), jpeg_quality 12, fb_count 1. fb_location and grab_mode are not written on this path (they keep stack values).

After a successful init, the sketch calls `esp_camera_sensor_get()` and then four sensor methods. The names come from sensor_t vtable offsets, so confidence is medium:
- offset 116, `set_vflip(1)`
- offset 112, `set_hmirror(1)`
- offset 64, `set_framesize(10 = VGA)`
- offset 92, `set_quality(15)`

Setup then prints "Camera OK" or "Camera Init Failed!" to the USB console.

All of these GPIOs are valid on the S3 and avoid 19/20 (USB) and 26–37 (flash/PSRAM).

## 3. Network / routes
**Startup flow:**
1. Preferences `wifi` is opened and `ssid`/`pass` are read.
2. If nothing is saved, the board starts the AP portal.
3. Otherwise it calls `WiFi.begin` and waits up to about 10 s (`millis` delta 9999, with 500 ms waits).
4. If that fails, it starts the AP portal.
5. On success it prints "Connected! IP: %s", calls `MDNS.begin("conqueror")` (probable; the hostname string is `conqueror`), starts **UDP on port 8888** (0x22b8, 0x42003639), and starts the camera httpd servers (0x42003c3c).

**AP setup portal (AP mode only):**
- `softAP(<ssid literal>, NULL, ch 1, hidden 0, max 4)`. The AP is **open** (no password).
- DNSServer on port 53 with `"*"`, so it acts as a captive portal.
- Arduino `WebServer` on port **8080** (0x1f90).
- Routes:
  - `/` serves the "Conqueror Setup" HTML form.
  - `/save` (POST) takes args `s` (SSID) and `p` (password), stores them in NVS, replies "Saved. Rebooting...", and reboots.
  - `onNotFound` redirects to `http://<ip>:8080/`.
- loop calls `handleClient` only while `WiFi.getMode()==AP`.

**Camera httpd (esp_http_server), two instances:**
- Server 1 runs on the **default port 80** (stack 8192). It has `GET /stream`, an MJPEG `multipart/x-mixed-replace` stream that uses the `fps` setting.
- Server 2 runs on **port 81** (config word 0x80010051, so server_port 0x51 and ctrl_port 0x8001). It has two routes:
  - `GET /control` (handler 0x42003a18).
  - `GET /cam_status` (handler 0x42003bcc), which returns `{"resolution":"%s","quality":%d,"fps":%d}` with `Access-Control-Allow-Origin: *`.
- Initial globals, from DRAM 0x3fc97d00: quality 10, framesize 10 (VGA), fps 30.

## Control interface
**`/control` is camera settings only. It does not drive the tank.** It reads three query keys through a helper at 0x420039c4 (query string plus key lookup):
- `res`: one of `QVGA, CIF, HVGA, VGA, SVGA, XGA, HD, SXGA, UXGA` (framesize 6, 8, 9, 10, 11, 12, 13, 14, 15; anything else gives VGA). The handler calls `s->set_framesize` and stores the global.
- `quality`: accepted when it is 5–63 (`v-5 <= 58`). It goes to the sensor and the global (that block disassembles poorly; the store is probable).
- `fps`: accepted when it is 1–30. It is stored to the global used by `/stream`.
- The reply is `{"resolution":"%s","quality":%d,"fps":%d,"changed":true|false}` as `application/json` with CORS `*`. If the sensor is missing, it returns HTTP 500 "Camera sensor error".
- **It writes nothing to Serial1.**

**The motion and command channel is a raw UDP bridge on port 8888.** It works both ways and passes data through untouched:
- **Downlink:** loop calls `udp.parsePacket()`, then `udp.read(buf, 254)`, null-terminates the buffer, calls **`Serial1.println(buf)`** (the packet text plus CRLF), and logs `[DOWN] %s` on USB.
- **Uplink:** when Serial1 has data, loop calls `Serial1.readStringUntil('\n')`, then `udp.beginPacket(udp.remoteIP(), 8888)`, `udp.print(line)`, `endPacket`, and logs `[UP] %s`. Replies go to the last sender's IP on port 8888 (high confidence on 8888; medium confidence on the remote-IP source).
- The ESP builds no JSON itself. There are no `{"N"`, `var`/`val`/`cmd` or `D1` strings anywhere in the app strings. The Uno-side protocol (for example ELEGOO `{"N":..}`) is whatever the UDP client sends.
- **No STOP and no timeout is generated by the ESP.** On WiFi loss, loop clears a "connected" flag and retries the WiFi connection every 5 s (0x1388), but it sends nothing to the Uno. Any deadman behaviour therefore has to live on the Uno.
- No WebSocket and no TCP command server were found. Listening ports are 80 (stream), 81 (control/status), 8888/UDP (command bridge), 32768/32769 (httpd control sockets), and 8080 plus 53/UDP in AP mode only.

**Diagnostics:** every 3 s (0xbb8) the board prints `[DBG] Serial1 avail:%d WiFi:%s` (Serial1.available(), local IP) to USB.

## Other pins
I found no `pinMode`/`digitalWrite`, LED or ledc usage in the sketch code. The only pins it configures are the camera pins and UART1 (3/40). The camera XCLK uses LEDC timer 0 / channel 0.

## Confidence summary
- UART 115200 8N1, RX 3 / TX 40: **high**. The Serial1 identity rests on the debug string and object usage.
- Camera pin map: **high**, an exact match to the ESP32-S3-EYE.
- Routes and ports (80 /stream; 81 /control, /cam_status; 8080 portal; UDP 8888): **high**.
- Sensor method names (vflip, hmirror, framesize, quality): **medium**. The quality store in `/control` is **medium**.
