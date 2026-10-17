"""Debug-metrics recorder over WiFi (bridge LLD R5): no USB cable, no S1 flipping.

Connects to the bridge as a viewer (it never takes control), asks for the debug stream
(`{"t":"debug","on":true,"hz":10|50}`), writes one CSV row per `td` message with the latest `tds`
values as extra columns, prints a summary and switches the stream off again.

    uv run python td_record.py --hz 50 --seconds 10 --out run.csv
"""

from __future__ import annotations

import argparse
import csv
import time
from pathlib import Path
from typing import Any

from bridge_echo import WsClient

TD_COLUMNS = [
    "t_s",
    "seq",
    "uno_ms",
    "enc_l",
    "enc_r",
    "pwm_l",
    "pwm_r",
    "cmd_l",
    "cmd_r",
    "act_l",
    "act_r",
    "yaw",
    "rate",
    "pitch",
    "roll",
    "dist_mm",
    "dist_fault",
    "closing",
    "pan",
    "tilt",
    "flags",
    "prim",
    "pct",
]
TDS_COLUMNS = [
    "batt_mv",
    "loop_load",
    "loop_max_us",
    "free_ram",
    "err_frames",
    "err_i2c",
    "flags2",
    "hw",
]


def td_row(msg: dict[str, Any], t_s: float, slow: dict[str, Any]) -> list[Any]:
    enc, pwm, cmd, act = msg["enc"], msg["pwm"], msg["cmd"], msg["act"]
    row: list[Any] = [
        f"{t_s:.3f}",
        msg["seq"],
        msg["uno_ms"],
        enc[0],
        enc[1],
        pwm[0],
        pwm[1],
        cmd[0],
        cmd[1],
        act[0],
        act[1],
        msg["yaw"],
        msg["rate"],
        msg["pitch"],
        msg["roll"],
        msg["dist_mm"],
        int(bool(msg.get("dist_fault", False))),
        msg["closing"],
        msg["pan"],
        msg["tilt"],
        msg["flags"],
        msg["prim"],
        msg["pct"],
    ]
    return row + [slow.get(k, "") for k in TDS_COLUMNS]


def wrap_deg(d: float) -> float:
    return (d + 540.0) % 360.0 - 180.0


def record(host: str, hz: int, seconds: float, out: Path) -> int:
    ws = WsClient(host)
    ws.send({"t": "hello", "client": "td-recorder", "proto": 1})
    ws.send({"t": "debug", "on": True, "hz": hz, "id": 1})
    rows: list[dict[str, Any]] = []
    slow: dict[str, Any] = {}
    start = time.monotonic()
    with out.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(TD_COLUMNS + TDS_COLUMNS)
        while time.monotonic() - start < seconds:
            msg = ws.recv(0.5)
            if msg is None:
                continue
            kind = msg.get("t")
            if kind == "tds":
                slow = msg
            elif kind == "td":
                writer.writerow(td_row(msg, time.monotonic() - start, slow))
                rows.append(msg)
            elif kind == "nack":
                print(f"bridge refused: {msg.get('reason')} {msg.get('message', '')}")
                ws.close()
                return 1
    ws.send({"t": "debug", "on": False})
    ws.close()
    if len(rows) < 2:
        print("no td messages received (bridge without LLD R5 firmware, or the Uno is offline)")
        return 1
    first, last = rows[0], rows[-1]
    span_s = (last["uno_ms"] - first["uno_ms"]) / 1000.0
    d_l, d_r = last["enc"][0] - first["enc"][0], last["enc"][1] - first["enc"][1]
    pwm_l = [r["pwm"][0] for r in rows]
    pwm_r = [r["pwm"][1] for r in rows]
    print(f"{len(rows)} td rows in {span_s:.1f} s ({len(rows) / max(span_s, 1e-3):.1f}/s) -> {out}")
    print(
        f"encoders: L {d_l:+d}  R {d_r:+d} counts  ({d_l / max(span_s, 1e-3):.0f} / "
        f"{d_r / max(span_s, 1e-3):.0f} per s)"
    )
    print(
        f"heading change {wrap_deg(last['yaw'] - first['yaw']):+.1f} deg; "
        f"pwm L {min(pwm_l)}..{max(pwm_l)}  R {min(pwm_r)}..{max(pwm_r)}"
    )
    if slow:
        print(
            f"uno: batt {slow['batt_mv']} mV, loop load {slow['loop_load']} %, "
            f"loop max {slow['loop_max_us']} us, free RAM {slow['free_ram']} B"
        )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Record the bridge debug stream (td / tds) to CSV")
    ap.add_argument("--host", default="conqueror-bridge.local")
    ap.add_argument("--hz", type=int, choices=[10, 50], default=50)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--out", type=Path, default=Path("td_run.csv"))
    args = ap.parse_args()
    return record(args.host, args.hz, args.seconds, args.out)


if __name__ == "__main__":
    raise SystemExit(main())
