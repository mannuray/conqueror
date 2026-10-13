#!/usr/bin/env python3
"""Stand/floor smoke tool for the Uno body firmware (Plan B). Plays the ESP32's role.

Run from tools/smoke/:  uv run python uno_smoke.py --port PORT CMD ...
"""

from __future__ import annotations

import argparse
import array
import csv
import math
import statistics
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


def run_script(
    link: Link, script: g.CalScript, param: int, csv_path: str | None = None, timeout: float = 30.0
) -> list[g.CalData]:
    samples: list[g.CalData] = []
    done = threading.Event()
    status: list[str] = []

    def on(msg: object) -> None:
        if isinstance(msg, g.CalData):
            samples.append(msg)
        elif isinstance(msg, g.CalResult):
            st = g.ACK_STATUS[msg.status]
            name = g.CalScript(msg.script).name
            print(f"CAL_RESULT {name} stage={msg.stage} {msg.progress}% {st}")
            if msg.progress == 100 or st != "OK":
                status.append(st)
                done.set()
        elif isinstance(msg, g.Ack) and msg.status != 0:
            print(f"ACK {g.ACK_STATUS[msg.status]}")
            status.append(g.ACK_STATUS[msg.status])
            done.set()

    link.handlers.append(on)
    wait_synced(link)
    link.send(g.Calibrate(op=g.CalOp.START, script=script, param=param, profile=0))
    done.wait(timeout)
    link.handlers.remove(on)
    if csv_path:
        with open(csv_path, "w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["t_ms", "phase", *[f"v{i}" for i in range(8)]])
            for s in samples:
                w.writerow([s.t_ms, s.phase, *s.v])
    print(f"{len(samples)} samples, result {status[0] if status else 'TIMEOUT'}")
    return samples


def cmd_calibrate(link: Link, args: argparse.Namespace) -> None:
    run_script(link, g.CalScript[args.script.upper()], args.param, args.csv)


def ask(question: str) -> bool:
    return input(f"{question} [y/n] ").strip().lower().startswith("y")


def cmd_selftest(link: Link, args: argparse.Namespace) -> None:
    """Self-test re-run (LLD §10.9), all in ONE process.

    Opening the port reboots the Uno (R13), and the boot run (due 2 s after reset) must finish
    first — a SELF_TEST sent earlier gets BUSY.
    """
    results: list[g.Event] = []

    def on(msg: object) -> None:
        if isinstance(msg, g.Event):
            print(fmt_event(msg))
            if msg.kind == int(g.EventKind.SELFTEST):
                results.append(msg)

    def wait_result(n: int, timeout: float) -> g.Event | None:
        end = time.monotonic() + timeout
        while len(results) < n and time.monotonic() < end:
            time.sleep(0.05)
        return results[n - 1] if len(results) >= n else None

    # DRIVE is streamed (no ACK): a refusal shows as EVENT REJECTED
    def drive(seconds: float) -> None:
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            link.send(g.Drive(src=g.Source.HUMAN, v=40, w=0))
            fast = link.last.get("TelemFast")
            if isinstance(fast, g.TelemFast):
                print(f"pwm={fast.pwm_l},{fast.pwm_r}")
            time.sleep(0.05)
        link.send(g.Stop(src=g.Source.HUMAN))

    link.handlers.append(on)
    link.handlers.append(print_ack)
    wait_synced(link)
    boot = wait_result(1, 5.0)
    if boot is None:
        print("no boot EVENT SELFTEST")
        return
    if args.lock_check:
        if not boot.b:
            print("boot run not locked: switch the battery off (Uno on USB only) and run again")
            return
        drive(1.0)  # expect EVENT REJECTED (BLOCKED) and pwm=0,0
        # ACK BLOCKED
        link.send(g.Calibrate(op=g.CalOp.START, script=g.CalScript.PWM_RAMP, param=0, profile=0))
        time.sleep(0.3)
        input("Switch the battery on, then Enter…")
        # battery-only lock: no re-run needed; EVENT SELFTEST b=0 arrives ~1 s after the
        # battery reads valid
        auto = wait_result(2, 5.0)
        if auto is None or auto.b:
            print("lock did not clear automatically")
            return
        drive(1.0)  # unlocked automatically (no reboot, no re-run): the tracks turn
        return
    link.send(g.Calibrate(op=g.CalOp.START, script=g.CalScript.SELF_TEST, param=0, profile=0))  # OK
    wait_result(2, 5.0)


def cmd_probe(link: Link, args: argparse.Namespace) -> None:
    """Shield probe (tank on the stand): STBY on D3 + direction pin polarity per track."""
    results: dict[str, bool] = {}
    # PWM_RAMP param: track (0 left, 1 right) | reverse << 1 | max PWM << 8
    for name, param in (
        ("left forward", 0x00),
        ("left reverse", 0x02),
        ("right forward", 0x01),
        ("right reverse", 0x03),
    ):
        track = name.split()[0]
        input(f"Watch the {track} track, Enter runs '{name}' (ramp to PWM 120 and back)…")
        samples = run_script(link, g.CalScript.PWM_RAMP, param | (120 << 8), timeout=15)
        edges = max((s.v[2] for s in samples), default=0)  # |encoder counts| of the selected track
        moved = ask(f"Did the {name.split()[0]} track move {name.split()[1]}?")
        results[name] = moved
        print(f"  encoder edges seen: {edges}")
    print("PROBE RESULT:", "PASS" if all(results.values()) else f"FAIL {results}")
    if not all(results.values()):
        print("Nothing moved → STBY (D3) wrong. Moved the wrong way → flip kDir*ForwardHigh.")


def cmd_mount(link: Link, args: argparse.Namespace) -> None:
    """IMU mount check: level, then nose up, then CCW rotation by hand."""
    input("Tank level and still. Enter…")
    s = run_script(link, g.CalScript.IDLE, 2000)
    if not s:
        print("no samples")
        return
    ax, ay, az = (statistics.mean(x.v[i] for x in s) / 8192 for i in (3, 4, 5))
    print(f"level: ax={ax:+.2f} g ay={ay:+.2f} g az={az:+.2f} g  (expect 0, 0, +1)")
    input("Lift the FRONT ~3 cm and hold. Enter…")
    s = run_script(link, g.CalScript.IDLE, 1500)
    if not s:
        print("no samples")
        return
    ax2 = statistics.mean(x.v[3] for x in s) / 8192
    print(f"nose up: ax={ax2:+.2f} g  (expect positive: specific force, +sin(pitch))")
    input("Put it down. Press Enter, then rotate the tank CCW (seen from above) by hand for 3 s…")
    s = run_script(link, g.CalScript.IDLE, 3000)
    gz = max((x.v[2] for x in s), key=abs, default=0) / 65.5
    print(f"CCW rotation: peak gz={gz:+.0f} °/s  (expect positive)")
    ok = az > 0.8 and ax2 > 0.03 and gz > 20
    print("MOUNT CHECK:", "PASS" if ok else "FAIL → adjust kImuMapX/Y/Z in board_config.h")


def cmd_bias(link: Link, args: argparse.Namespace) -> None:
    """Reference calculator: gyro bias + level offsets from a 3 s still sample, written + saved."""
    input("Tank on a level floor, motors off, do not touch. Enter…")
    s = run_script(link, g.CalScript.STILL_SAMPLE, 3000)
    if not s:
        print("no samples")
        return
    gx, gy, gz = (round(statistics.mean(x.v[i] for x in s)) for i in (0, 1, 2))
    ax, ay, az = (statistics.mean(x.v[i] for x in s) / 8192 for i in (3, 4, 5))

    pitch = math.degrees(math.atan2(ax, math.hypot(ay, az)))  # same convention as Attitude (Task 4)
    roll = math.degrees(math.atan2(ay, az))
    values = {
        g.SettingId.GYRO_BIAS_X: gx,
        g.SettingId.GYRO_BIAS_Y: gy,
        g.SettingId.GYRO_BIAS_Z: gz,
        g.SettingId.LEVEL_PITCH: round(pitch * 100),
        g.SettingId.LEVEL_ROLL: round(roll * 100),
    }
    for sid, v in values.items():
        link.send(g.SettingSet(id=sid, value=v))
        print(f"{sid.name.lower()} = {v}")
        time.sleep(0.05)
    link.send(g.SettingsSave())
    time.sleep(1.5)


COMMANDS: dict[str, Callable[[Link, argparse.Namespace], None]] = {"watch": cmd_watch}
COMMANDS["setting"] = cmd_setting
COMMANDS.update(
    {
        "drive": cmd_drive,
        "stop": cmd_stop,
        "envelope": cmd_envelope,
        "deadman": cmd_deadman,
        "override": cmd_override,
        "calibrate": cmd_calibrate,
        "probe": cmd_probe,
        "mount": cmd_mount,
        "bias": cmd_bias,
        "selftest": cmd_selftest,
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
    c = sub.add_parser("calibrate")
    c.add_argument("script", choices=[s.name.lower() for s in g.CalScript])
    c.add_argument("--param", type=int, default=0)
    c.add_argument("--csv")
    sub.add_parser("probe")
    sub.add_parser("mount")
    sub.add_parser("bias")
    st = sub.add_parser("selftest")
    st.add_argument(
        "--lock-check",
        action="store_true",
        help="stand check 5a: locked boot → refusals → automatic unlock",
    )
    args = ap.parse_args()
    link = Link(args.port)
    try:
        COMMANDS[args.cmd](link, args)
    finally:
        link.close()


if __name__ == "__main__":
    main()
