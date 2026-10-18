#!/usr/bin/env python3
"""Plan C bridge smoke checks over WiFi (developer bridge, no authentication).

Run from tools/smoke/:
  uv run python bridge_echo.py --host conqueror-bridge.local no-tcp
  uv run python bridge_echo.py --host conqueror-bridge.local ws --seconds 5
  uv run python bridge_echo.py --host conqueror-bridge.local ws-owner
None of the checks moves the tank: the only drive messages are v=0, w=0.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import socket
import struct
import time
from typing import Any


OLD_BENCH_PORT = 3333  # removed from the bridge (design-notes §74); `no-tcp` checks it stays closed
HTTP_PORT = 80


class WsClient:
    """Tiny RFC 6455 text-frame client (stdlib only) for the bridge's /ws."""

    def __init__(self, host: str) -> None:
        self.sock = socket.create_connection((host, HTTP_PORT), timeout=5.0)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET /ws HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(request.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                raise ConnectionError("bridge closed the WebSocket upgrade")
            head += chunk
        if b" 101 " not in head.split(b"\r\n", 1)[0]:
            raise ConnectionError(f"upgrade refused: {head[:40]!r}")

    def send(self, obj: dict[str, Any]) -> None:
        payload = json.dumps(obj, separators=(",", ":")).encode()
        mask = os.urandom(4)
        if len(payload) < 126:
            header = struct.pack("!BB", 0x81, 0x80 | len(payload))
        else:
            header = struct.pack("!BBH", 0x81, 0x80 | 126, len(payload))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def _exact(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("WebSocket closed")
            data += chunk
        return data

    def recv(self, timeout: float) -> dict[str, Any] | None:
        """Next JSON message, or None on timeout."""
        self.sock.settimeout(timeout)
        try:
            b0, b1 = self._exact(2)
        except TimeoutError:
            return None
        self.sock.settimeout(5.0)
        length = b1 & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._exact(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._exact(8))
        payload = self._exact(length)
        opcode = b0 & 0x0F
        if opcode == 0x8:
            raise ConnectionError("WebSocket closed by the bridge")
        if opcode != 0x1:
            return self.recv(timeout)
        message: dict[str, Any] = json.loads(payload)
        return message

    def wait_for(self, kind: str, seconds: float) -> dict[str, Any] | None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            msg = self.recv(max(0.01, end - time.monotonic()))
            if msg is not None and msg.get("t") == kind:
                return msg
        return None

    def close(self) -> None:
        self.sock.close()


def cmd_ws(host: str, seconds: float) -> int:
    ws = WsClient(host)
    ws.send({"t": "hello", "client": "bridge-echo", "proto": 1})
    welcome = ws.wait_for("welcome", 2.0)
    print(f"welcome: {welcome}")
    ws.send({"t": "ping", "t0": 12345})
    pong = ws.wait_for("pong", 2.0)
    print(f"pong: {pong}")
    counts: dict[str, int] = {}
    last: dict[str, dict[str, Any]] = {}
    end = time.monotonic() + seconds
    next_ping = time.monotonic() + 1.0
    while time.monotonic() < end:
        if time.monotonic() >= next_ping:  # the bridge reaps clients silent for 3 s
            ws.send({"t": "ping", "t0": 1})
            next_ping += 1.0
        msg = ws.recv(0.1)
        if msg is None:
            continue
        kind = str(msg.get("t"))
        counts[kind] = counts.get(kind, 0) + 1
        last[kind] = msg
    print(f"messages in {seconds:.0f} s: {counts}")
    print(f"last tf: {last.get('tf')}")
    print(f"last ts: {last.get('ts')}")
    ws.send({"t": "stop", "id": 1})
    ack = ws.wait_for("ack", 1.0)
    print(f"stop -> {ack}")
    ws.send({"t": "turn", "id": 2})
    nack = ws.wait_for("nack", 1.0)
    print(f"turn -> {nack}")
    ws.close()
    tf_rate = counts.get("tf", 0) / seconds
    ok = (
        welcome is not None
        and pong is not None
        and pong.get("t0") == 12345
        and 7.0 <= tf_rate <= 12.0
        and counts.get("ts", 0) >= seconds - 1
        and ack is not None
        and ack.get("id") == 1
        and nack is not None
        and nack.get("reason") == "INVALID_COMMAND"
    )
    print(f"tf {tf_rate:.1f} Hz -> {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


def hold_zero_drive(ws: WsClient, seconds: float) -> None:
    """Owner by sending v=0, w=0 at 20 Hz (takes control, never moves the tank)."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ws.send({"t": "drive", "v": 0, "w": 0})
        ws.recv(0.05)


def cmd_ws_owner(host: str) -> int:
    phone = WsClient(host)
    phone.send({"t": "hello", "client": "bridge-echo", "proto": 1})
    phone.wait_for("welcome", 2.0)
    phone.wait_for("ts", 2.0)
    hold_zero_drive(phone, 1.0)
    other = WsClient(host)
    other.send({"t": "hello", "client": "bridge-echo-2", "proto": 1})
    other.wait_for("welcome", 2.0)
    other.send({"t": "drive", "v": 0, "w": 0})
    not_owner = other.wait_for("nack", 1.0)
    print(f"second phone drive -> {not_owner}")
    other.close()
    phone.send({"t": "release_control", "id": 9})
    released = phone.wait_for("ack", 1.0)
    phone.close()
    ok = not_owner is not None and not_owner.get("reason") == "NOT_OWNER" and released is not None
    print("owner rules OK" if ok else "owner rules FAILED")
    return 0 if ok else 1


def cmd_no_tcp(host: str) -> int:
    """The old bench port must refuse connections (design-notes §74)."""
    try:
        sock = socket.create_connection((host, OLD_BENCH_PORT), timeout=3.0)
    except OSError as e:
        print(f"port {OLD_BENCH_PORT} closed: {type(e).__name__}")
        return 0
    sock.close()
    print(f"port {OLD_BENCH_PORT} is OPEN - the bench passthrough is still there")
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="conqueror-bridge.local")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("no-tcp", help="the removed bench port 3333 refuses connections")
    ws = sub.add_parser("ws", help="WebSocket welcome, pong, telemetry rates, stop ack, nack")
    ws.add_argument("--seconds", type=float, default=5.0)
    sub.add_parser("ws-owner", help="phone owns -> 2nd phone NOT_OWNER, release")
    args = parser.parse_args()
    host: str = args.host
    if args.cmd == "no-tcp":
        return cmd_no_tcp(host)
    if args.cmd == "ws":
        return cmd_ws(host, float(args.seconds))
    return cmd_ws_owner(host)


if __name__ == "__main__":
    raise SystemExit(main())
