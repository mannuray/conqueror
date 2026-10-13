#!/usr/bin/env python3
"""Plan C bridge smoke checks over WiFi (developer bridge, no authentication).

Run from tools/smoke/:
  uv run python bridge_echo.py --host conqueror-bridge.local tcp --seconds 10
  uv run python bridge_echo.py --host conqueror-bridge.local tcp-busy
"""

from __future__ import annotations

import argparse
import socket
import threading
import time

from conqueror_protocol import FrameParser, build_frame, decode_frame
from conqueror_protocol import generated as g
from conqueror_protocol.wire import Message

TCP_PORT = 3333
SYNCED_BIT = 1 << g.FastFlagBit.SYNCED


class TcpBench:
    """Minimal bench: heartbeats, HELLO_REQ -> HELLO -> HELLO_ACK, frame counting."""

    def __init__(self, host: str) -> None:
        self.sock = socket.create_connection((host, TCP_PORT), timeout=5.0)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(0.05)
        self.parser = FrameParser()
        self.seq = 0
        self.lock = threading.Lock()
        self.running = True
        self.closed_by_peer = False
        self.hello: g.Hello | None = None
        self.counts: dict[str, int] = {}
        self.synced_seen = False
        threading.Thread(target=self._heartbeat, daemon=True).start()

    def send(self, msg: Message) -> None:
        with self.lock:
            frame = build_frame(msg, self.seq)
            self.seq = (self.seq + 1) & 0xFF
            self.sock.sendall(frame)

    def _heartbeat(self) -> None:
        while self.running:
            try:
                self.send(g.Heartbeat(esp_ms=int(time.monotonic() * 1000) & 0xFFFFFFFF))
            except OSError:
                return
            time.sleep(0.1)

    def pump(self, seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end and self.running:
            try:
                data = self.sock.recv(1024)
            except TimeoutError:
                continue
            if not data:
                self.closed_by_peer = True
                return
            for raw in self.parser.feed(data):
                msg = decode_frame(raw, "uno_to_esp32")
                name = type(msg).__name__
                self.counts[name] = self.counts.get(name, 0) + 1
                self._on(msg)

    def _on(self, msg: Message) -> None:
        if isinstance(msg, g.Hello):
            self.hello = msg
            print(
                f"HELLO fw={msg.fw >> 8}.{msg.fw & 0xFF} proto={msg.proto} "
                f"commit={msg.commit:08x} -> HELLO_ACK sent"
            )
            self.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
        elif isinstance(msg, g.TelemFast) and msg.flags & SYNCED_BIT:
            self.synced_seen = True

    def close(self) -> None:
        self.running = False
        self.sock.close()


def cmd_tcp(host: str, seconds: float) -> int:
    bench = TcpBench(host)
    bench.send(g.HelloReq())
    bench.pump(seconds)
    bench.close()
    if bench.closed_by_peer:
        print("closed by bridge (busy: a phone is driving, or another bench is connected)")
        return 1
    stats = bench.parser.stats
    print(f"frames by type: {dict(sorted(bench.counts.items()))}")
    print(f"done: frames={stats.frames} bad_crc={stats.bad_crc} synced={bench.synced_seen}")
    ok = bench.hello is not None and bench.hello.proto == g.PROTOCOL_VERSION
    return 0 if ok and bench.synced_seen and stats.bad_crc == 0 else 1


def cmd_tcp_busy(host: str) -> int:
    first = TcpBench(host)
    first.send(g.HelloReq())
    first.pump(0.5)
    second = socket.create_connection((host, TCP_PORT), timeout=5.0)
    second.settimeout(2.0)
    try:
        refused = second.recv(64) == b""
    except TimeoutError:
        refused = False
    second.close()
    first.close()
    print(f"second bench refused: {refused}")
    return 0 if refused else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="conqueror-bridge.local")
    sub = parser.add_subparsers(dest="cmd", required=True)
    tcp = sub.add_parser("tcp", help="bench handshake through the TCP passthrough")
    tcp.add_argument("--seconds", type=float, default=10.0)
    sub.add_parser("tcp-busy", help="a second bench is accepted and closed at once")
    args = parser.parse_args()
    host: str = args.host
    if args.cmd == "tcp":
        return cmd_tcp(host, float(args.seconds))
    return cmd_tcp_busy(host)


if __name__ == "__main__":
    raise SystemExit(main())
