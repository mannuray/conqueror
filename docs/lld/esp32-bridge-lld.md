# ESP32 WiFi Bridge (developer firmware) — Low-Level Design

Status: APPROVED · Date: 2026-10-11 · Roadmap phase 3 · `firmware/esp32` → `env:bridge` (Arduino-ESP32 3.x); later only in gateway debug builds (`-D CQ_DEV_BRIDGE`)

## Class diagram
```mermaid
classDiagram
    direction LR
    class BridgeMain { +setup() +loop() }
    class WifiLink {
      -State state_
      +begin(Secrets)
      +step(now)
      +ip() IPAddress
    }
    class Secrets { <<secrets.h, git-ignored>> +ssid +password +ap_password +hostname }
    class MdnsAdvertiser { +start(hostname) }
    class TcpBridge {
      -WiFiServer server_
      -WiFiClient client_
      +poll()
      +pumpTcpToUart()
      +pumpUartToTcp()
      +stats() BridgeStats
    }
    class UartPort { <<Serial2>> IO33 RX · IO4 TX · 250000 · 1 KB buffers }
    class CameraStream { <<optional>> ELEGOO handler :81 /stream, 1 viewer }
    class StatusIndicator { <<IO13 LED if fitted (board V1.2) + USB serial log>> }
    class BridgeStats { +bytes_up +bytes_down +clients +drops }
    BridgeMain --> WifiLink
    BridgeMain --> MdnsAdvertiser
    BridgeMain --> TcpBridge
    BridgeMain --> CameraStream
    BridgeMain --> StatusIndicator
    WifiLink ..> Secrets
    TcpBridge --> UartPort
    TcpBridge ..> BridgeStats
```
`WifiLink.State {Connecting, Station, FallbackAp}`; mDNS `conqueror-bridge.local`, service `_conqueror-bridge._tcp`; TCP port 3333 with TCP_NODELAY.

## WiFi behaviour
```mermaid
stateDiagram-v2
    [*] --> Connecting : boot (home network from secrets.h)
    Connecting --> Station : connected → mDNS, print IP on USB serial
    Connecting --> FallbackAp : 30 s → WPA2 AP conqueror-bridge
    Station --> Connecting : link lost (retry)
    FallbackAp --> Connecting : retry home network every 60 s (only if no client)
```
Status: always logged on ESP32 USB serial and shown by the bench tool; **IO13 green LED only on board V1.2** (V1.0 has no LED there): fast blink connecting · solid station · slow blink fallback AP · flicker on traffic.

## Byte pump
- Transparent both ways; frames not parsed (protocol + CRC end to end).
- One client; a new connection replaces the old (logged).
- TCP_NODELAY, immediate flush (~5–20 ms latency on home WiFi).
- **Never generates heartbeats or commands** → laptop/WiFi loss = heartbeats stop = Uno halts within 300 ms.
- Uno bytes with no client are discarded.

```mermaid
sequenceDiagram
    participant L as Laptop (bench, TcpTransport)
    participant B as ESP32 bridge
    participant U as Uno
    L->>B: TCP connect conqueror-bridge.local:3333
    loop every 100 ms
        L->>B: HEARTBEAT bytes
        B->>U: same bytes (UART 250000)
    end
    U->>B: HELLO / TELEM bytes
    B->>L: same bytes
    L->>B: MOVE frame
    B->>U: MOVE frame
    Note over L,B: laptop WiFi drops
    U->>U: no frame 300 ms → link lost → STOP, move cancelled
```

## Optional camera
ELEGOO's stream handler on :81 `/stream` (single viewer) for watching tests; proper multi-viewer camera comes with the gateway.

## Security
No authentication — developer use on the home network only; stated in README and boot log; never in release firmware.

## Hardware notes (verified from schematics)
- ESP32 board revisions V1.0 and V1.2: identical UART (IO33 RX, IO4 TX on connector P1), camera connector and power; only V1.2 fits the IO13 green LED.
- Uno shield = Conqueror **tank** shield (DRV8835, PWM D5/D6, PH D8/D7, MODE high) — not the Smart Robot Car V4 shield (TB6612, STBY D8, AIN1 D9; those pins are the commented-out block in the ELEGOO firmware).
