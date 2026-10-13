#!/usr/bin/env python3
"""Stand/floor smoke tool for the Uno body firmware (Plan B). Plays the ESP32's role.

Run from tools/smoke/:  uv run python uno_smoke.py --port PORT CMD ...
"""

from __future__ import annotations

import argparse
import array
import struct
import sys
import termios
import threading
import time
from collections.abc import Callable

import serial  # pyserial
from conqueror_protocol import FrameParser, build_frame, decode_frame
from conqueror_protocol import generated as g

UNO = "uno_to_esp32"

# Linux: 250000 is not a named termios speed; pyserial's custom-speed path fails on some kernels
# (seen on 7.0 with a CH340: EINVAL). Fall back to setting it with the termios2 BOTHER ioctl.
_TCGETS2, _TCSETS2, _BOTHER, _CBAUD = 0x802C542A, 0x402C542B, 0o010000, 0o010017
_TERMIOS2 = "4IB19sII"  # struct termios2: iflag, oflag, cflag, lflag, line, cc[19], ispeed, ospeed


def open_serial(port: str, baud: int) -> serial.Serial:
    try:
        return serial.Serial(port, baud, timeout=0.01)
    except (OSError, termios.error):
        if not sys.platform.startswith("linux"):
            raise
    import fcntl

    ser = serial.Serial(port, 115200, timeout=0.01)
    fd = ser.fileno()
    buf = array.array("i", [0] * 64)
    fcntl.ioctl(fd, _TCGETS2, buf, True)
    size = struct.calcsize(_TERMIOS2)
    iflag, oflag, cflag, lflag, line, cc, _isp, _osp = struct.unpack(
        _TERMIOS2, buf.tobytes()[:size]
    )
    cflag = (int(cflag) & ~_CBAUD) | _BOTHER
    fcntl.ioctl(
        fd, _TCSETS2, struct.pack(_TERMIOS2, iflag, oflag, cflag, lflag, line, cc, baud, baud)
    )
    return ser


class Link:
    def __init__(self, port: str) -> None:
        self.ser = open_serial(port, g.BAUD)
        self.parser = FrameParser()
        self.seq = 0
        self.lock = threading.Lock()
        self.running = True
        self.handlers: list[Callable[[object], None]] = []
        self.synced = False
        self.last: dict[str, object] = {}
        # Opening the port resets the board; the Mega's stk500v2 bootloader stays in the bootloader
        # while bytes arrive, so stay silent until the firmware speaks (or 2 s pass).
        self.rx_seen = threading.Event()
        threading.Thread(target=self._rx, daemon=True).start()
        threading.Thread(target=self._heartbeat, daemon=True).start()

    def send(self, msg: object) -> int:
        self.rx_seen.wait(2.5)  # never talk into the bootloader window (see __init__)
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
                self.rx_seen.set()
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


def wait_synced(link: Link, timeout: float = 4.0) -> None:
    end = time.monotonic() + timeout
    while not link.synced and time.monotonic() < end:
        time.sleep(0.05)
    if not link.synced:
        print("no HELLO from the Uno - motion commands will be answered BUSY")
    time.sleep(0.1)  # HELLO_ACK reaches the control task within one tick


def print_ack(m: object) -> None:
    if isinstance(m, g.Ack):
        print(f"ACK {g.ACK_STATUS[m.status]}")


def cmd_drive(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(lambda m: print(fmt_event(m)) if isinstance(m, g.Event) else None)
    wait_synced(link)
    # same run: opening the port reboots the Uno, which clears the envelope
    if args.envelope is not None:
        link.send(g.Envelope(speed_cap=args.envelope, accel_cap=0, lock_brake=0))
        time.sleep(0.1)
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        link.send(g.Drive(src=g.Source.HUMAN, v=args.v, w=args.w))
        fast = link.last.get("TelemFast")
        if isinstance(fast, g.TelemFast):
            print(
                f"pwm={fast.pwm_l},{fast.pwm_r} enc={fast.enc_l},{fast.enc_r} "
                f"yaw={fast.yaw / 100:.1f} dist={fast.dist_mm} flags={fast.flags:#06x}"
            )
        time.sleep(0.05)


def cmd_stop(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(print_ack)
    wait_synced(link)
    link.send(g.Stop(src=g.Source.HUMAN))
    time.sleep(0.3)


def cmd_envelope(link: Link, args: argparse.Namespace) -> None:
    link.handlers.append(print_ack)
    wait_synced(link)
    link.send(g.Envelope(speed_cap=args.cap, accel_cap=args.accel, lock_brake=args.lock))
    time.sleep(0.3)


def cmd_deadman(link: Link, args: argparse.Namespace) -> None:
    """Drive 30 % for 1 s, then stop streaming DRIVE (heartbeats go on): expect DEADMAN_STOP."""
    link.handlers.append(lambda m: print(fmt_event(m)) if isinstance(m, g.Event) else None)
    wait_synced(link)
    t0 = time.monotonic()
    for _ in range(20):
        link.send(g.Drive(src=g.Source.HUMAN, v=30, w=0))
        t0 = time.monotonic()  # time of the last DRIVE
        time.sleep(0.05)
    while time.monotonic() - t0 < 1.5:
        fast = link.last.get("TelemFast")
        if isinstance(fast, g.TelemFast) and fast.pwm_l == 0 and fast.pwm_r == 0:
            print(f"motors stopped {int((time.monotonic() - t0) * 1000)} ms after the last DRIVE")
            break
        time.sleep(0.01)
    time.sleep(max(0.0, t0 + 1.3 - time.monotonic()))  # keep printing events: idle release at ~1 s


def cmd_override(link: Link, args: argparse.Namespace) -> None:
    """SAFETY_OVERRIDE on/off (LLD §10). --ai sends src=AI (must be refused: ROLE_FORBIDDEN)."""
    link.handlers.append(print_ack)
    link.handlers.append(lambda m: print(fmt_event(m)) if isinstance(m, g.Event) else None)
    wait_synced(link)
    src = g.Source.AI if args.ai else g.Source.HUMAN
    link.send(g.SafetyOverride(src=src, on=1 if args.state == "on" else 0))
    time.sleep(0.3)
    # same run: reopening the port reboots the Uno
    if args.state == "on" and args.drive is not None:
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            link.send(g.Drive(src=g.Source.HUMAN, v=args.drive, w=0))
            fast = link.last.get("TelemFast")
            if isinstance(fast, g.TelemFast):
                print(
                    f"pwm={fast.pwm_l},{fast.pwm_r} dist={fast.dist_mm} "
                    f"pitch={fast.pitch / 100:.1f}"
                )
            time.sleep(0.05)
        time.sleep(0.6)  # keep printing events: DEADMAN_STOP 300 ms after the last DRIVE


COMMANDS: dict[str, Callable[[Link, argparse.Namespace], None]] = {"watch": cmd_watch}
COMMANDS["setting"] = cmd_setting
COMMANDS.update(
    {
        "drive": cmd_drive,
        "stop": cmd_stop,
        "envelope": cmd_envelope,
        "deadman": cmd_deadman,
        "override": cmd_override,
    }
)


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
    d = sub.add_parser("drive")
    d.add_argument("v", type=int)
    d.add_argument("w", type=int)
    d.add_argument("--seconds", type=float, default=2.0)
    d.add_argument("--envelope", type=int, default=None, help="speed cap %% sent before driving")
    sub.add_parser("stop")
    e = sub.add_parser("envelope")
    e.add_argument("cap", type=int)
    e.add_argument("accel", type=int)
    e.add_argument(
        "lock", type=int, help="lock_brake bits: 1 lock brake, 3 child play (EnvelopeFlagBit)"
    )
    sub.add_parser("deadman")
    o = sub.add_parser("override")
    o.add_argument("state", choices=["on", "off"])
    o.add_argument("--ai", action="store_true", help="send src=AI (expect ROLE_FORBIDDEN)")
    o.add_argument("--drive", type=int, default=None, help="then drive at this %% (same run)")
    o.add_argument("--seconds", type=float, default=3.0)
    args = ap.parse_args()
    link = Link(args.port)
    try:
        COMMANDS[args.cmd](link, args)
    finally:
        link.close()


if __name__ == "__main__":
    main()
