#!/usr/bin/env python3
"""Stand/floor smoke tool for the Uno body firmware (Plan B). Plays the ESP32's role.

Run from tools/smoke/:  uv run python uno_smoke.py --port PORT CMD ...
"""

from __future__ import annotations

import argparse
import threading
import time
from collections.abc import Callable

import serial  # pyserial
from conqueror_protocol import FrameParser, build_frame, decode_frame
from conqueror_protocol import generated as g

UNO = "uno_to_esp32"


class Link:
    def __init__(self, port: str) -> None:
        self.ser = serial.Serial(port, g.BAUD, timeout=0.01)
        self.parser = FrameParser()
        self.seq = 0
        self.lock = threading.Lock()
        self.running = True
        self.handlers: list[Callable[[object], None]] = []
        self.synced = False
        self.last: dict[str, object] = {}
        threading.Thread(target=self._rx, daemon=True).start()
        threading.Thread(target=self._heartbeat, daemon=True).start()

    def send(self, msg: object) -> int:
        with self.lock:
            seq = self.seq
            self.seq = (self.seq + 1) & 0xFF
            self.ser.write(build_frame(msg, seq))  # type: ignore[arg-type]
            return seq

    def _heartbeat(self) -> None:
        while self.running:
            self.send(g.Heartbeat(esp_ms=int(time.monotonic() * 1000) & 0xFFFFFFFF))
            time.sleep(0.1)

    def _rx(self) -> None:
        while self.running:
            data = self.ser.read(256)
            for raw in self.parser.feed(data):
                try:
                    msg = decode_frame(raw, UNO)
                except ValueError as exc:
                    print(f"decode error: {exc}")
                    continue
                self.last[type(msg).__name__] = msg
                if isinstance(msg, g.Hello):
                    print(
                        f"HELLO fw={msg.fw:#06x} proto={msg.proto} caps={msg.caps} "
                        f"commit={msg.commit:08x} reset={g.ResetReason(msg.reset_reason).name}"
                    )
                    self.send(g.HelloAck(proto=g.PROTOCOL_VERSION))
                    self.synced = True
                for h in self.handlers:
                    h(msg)

    def close(self) -> None:
        self.send(g.Stop(src=g.Source.HUMAN))
        self.running = False
        time.sleep(0.15)
        self.ser.close()


def fmt_event(m: g.Event) -> str:
    try:
        name = g.EventKind(m.kind).name
    except ValueError:
        name = str(m.kind)
    return f"EVENT {name} a={m.a} b={m.b}"


def cmd_watch(link: Link, args: argparse.Namespace) -> None:
    def show(msg: object) -> None:
        if isinstance(msg, g.TelemFast):
            print(
                f"FAST yaw={msg.yaw / 100:.1f} rate={msg.yaw_rate / 10:.1f} "
                f"pitch={msg.pitch / 100:.1f} roll={msg.roll / 100:.1f} "
                f"pwm={msg.pwm_l},{msg.pwm_r} enc={msg.enc_l},{msg.enc_r} "
                f"dist={msg.dist_mm} flags={msg.flags:#06x} "
                f"pan={msg.pan / 10:.0f} tilt={msg.tilt / 10:.0f}"
            )
        elif isinstance(msg, g.TelemSlow):
            print(
                f"SLOW up={msg.uptime_s}s batt={msg.batt_mv}mV temp={msg.temp / 100:.1f}C "
                f"owner={g.Owner(msg.owner).name} load={msg.loop_load}% max={msg.loop_max_us}us "
                f"ram={msg.free_ram} errs={msg.err_frames} flags2={msg.flags2:#x}"
            )
        elif isinstance(msg, g.Event):
            print(fmt_event(msg))
        elif isinstance(msg, g.Ack):
            print(f"ACK seq={msg.ack_seq} {g.ACK_STATUS[msg.status]}")
        elif isinstance(msg, (g.WifiResetReq, g.AdminResetReq)):
            print(type(msg).__name__)

    link.handlers.append(show)
    time.sleep(args.seconds)


def cmd_setting(link: Link, args: argparse.Namespace) -> None:
    info = next(s for s in g.SETTINGS if s.name == args.name)
    done = threading.Event()

    def show(msg: object) -> None:
        if isinstance(msg, g.SettingValue) and msg.id == info.id:
            print(f"{info.name} = {msg.value} ({msg.value * info.scale:g} {info.unit})")
            done.set()
        elif isinstance(msg, g.Ack):
            print(f"ACK {g.ACK_STATUS[msg.status]}")
        elif isinstance(msg, g.Event):
            print(fmt_event(msg))

    link.handlers.append(show)
    time.sleep(0.6)
    if args.value is not None:
        link.send(g.SettingSet(id=info.id, value=args.value))
        time.sleep(0.2)
    link.send(g.SettingGet(id=info.id))
    done.wait(1.0)
    if args.save:
        link.send(g.SettingsSave())
        time.sleep(1.5)


COMMANDS: dict[str, Callable[[Link, argparse.Namespace], None]] = {"watch": cmd_watch}
COMMANDS["setting"] = cmd_setting


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("watch")
    w.add_argument("--seconds", type=float, default=10.0)
    s = sub.add_parser("setting")
    s.add_argument("name")
    s.add_argument("value", nargs="?", type=int)
    s.add_argument("--save", action="store_true")
    args = ap.parse_args()
    link = Link(args.port)
    try:
        COMMANDS[args.cmd](link, args)
    finally:
        link.close()


if __name__ == "__main__":
    main()
