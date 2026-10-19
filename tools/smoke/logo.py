"""Tank Logo from the laptop (Plan N3, Tank Logo LLD §20.4) until the /logo page (Plan N4) exists.

    uv run python logo.py check square.logo            # errors, your words, the planned path
    uv run python logo.py run square.logo [--step]     # check, run (takes control); Ctrl-C = STOP
    uv run python logo.py run --example avoid_walls
    uv run python logo.py list | load NAME | save NAME FILE | delete NAME

Runs move the tank: on the stand first (tracks off the ground). STOP from the phone or the IR
remote ends a run.
"""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import threading
import time
import urllib.parse
from collections.abc import Callable
from typing import Any, Protocol

from bridge_echo import WsClient

Message = dict[str, Any]


class WsLike(Protocol):
    def send(self, obj: dict[str, Any]) -> None: ...
    def recv(self, timeout: float) -> dict[str, Any] | None: ...
    def close(self) -> None: ...


def http_call(host: str, method: str, path: str, body: str | None = None) -> tuple[int, str]:
    conn = http.client.HTTPConnection(host, 80, timeout=10)
    try:
        data = body.encode() if body is not None else None
        conn.request(method, path, body=data, headers={"Content-Type": "text/plain; charset=utf-8"})
        r = conn.getresponse()
        return r.status, r.read().decode("utf-8", "replace")
    finally:
        conn.close()


def check_lines(reply: Message) -> list[str]:
    """The check reply in plain lines: OK / every error with its place, your words, the path."""
    out: list[str] = []
    for d in reply.get("diags", []):
        kind = "hint" if d.get("warning") else f"error {d['code']}"
        out.append(f"line {d['line']}, col {d['col']}: {kind}: {d['msg']}")
    for w in reply.get("words", []):
        gives = ", gives a value" if w["gives"] else ""
        out.append(f"your word {w['name']} ({w['inputs']} inputs{gives})")
    path: list[list[int]] = reply.get("path", [])
    if path:
        x, y, h = path[-1][0], path[-1][1], path[-1][2]
        uncertain = sum(1 for p in path if p[4])
        more = f" ({uncertain} uncertain)" if uncertain else ""
        out.append(
            f"path: {len(path)} points, ends at x {x / 10:.1f} cm, y {y / 10:.1f} cm, "
            f"heading {h}°{more}"
        )
    if reply.get("cut_short"):
        out.append("path cut short (a limit was reached)")
    if reply.get("sim_error"):
        e = reply["sim_error"]
        out.append(f"while planning: error {e['code']} on line {e['line']}: {e['msg']}")
    verdict = "OK - can run" if reply.get("ok") else "NOT runnable"
    out.append(f"{verdict} (program {reply.get('prog')})")
    return out


def event_line(ev: Message) -> str | None:
    """One logo_* event as text; None for anything else."""
    kind = ev.get("kind", "")
    if kind == "logo_started":
        return f"started {ev.get('name') or '(not saved)'} by {ev.get('by')}"
    if kind == "logo_line":
        return f"line {ev['line']}" + (f" (round {ev['round']})" if ev.get("round") else "")
    if kind == "logo_say":
        return f"SAY: {ev['text']}"
    if kind == "logo_note":
        return f"note on line {ev['line']}: {ev['msg']}"
    if kind == "logo_ended":
        reason = ev.get("reason")
        if reason == "finished":
            return "finished"
        return f"ended {reason} on line {ev['line']}: {ev['msg']}"
    return None


def run_program(
    ws: WsLike,
    prog: int,
    step: bool,
    show: Callable[[str], None],
    stepper: Callable[[], bool] | None = None,
    clock: Callable[[], float] = time.monotonic,
) -> Message | None:
    """logo_run, then the events until logo_ended (None: refused, or nothing for 330 s).

    Pings once a second whatever arrives, also while step mode waits for the next line (stepper is
    polled: True once Enter was pressed): the bridge closes a client that sends nothing for 3 s
    (ending the run with 7046)."""
    ws.send({"t": "logo_run", "prog": prog, "step": step, "id": 1})
    last_ping = last_heard = clock()
    waiting_step = False
    while True:
        if waiting_step and stepper is not None and stepper():
            ws.send({"t": "logo_step", "id": 2})
            waiting_step = False
        if clock() - last_ping >= 1.0:
            ws.send({"t": "ping", "t0": 0})
            last_ping = clock()
        m = ws.recv(1.0)
        if m is None:
            if clock() - last_heard > 330.0:
                return None
            continue
        last_heard = clock()
        if m.get("t") == "nack" and m.get("id") == 1:
            show(f"refused: {m.get('reason')} - {m.get('message')}")
            return None
        if m.get("t") != "ev":
            continue
        text = event_line(m)
        if text is not None:
            show(text)
        if m.get("kind") == "logo_ended":
            return m
        if step and m.get("kind") == "logo_line":
            waiting_step = True


class EnterKey:
    """Enter presses read by a background thread: the run loop never blocks and keeps pinging."""

    def __init__(self) -> None:
        self.pressed = threading.Event()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self) -> None:
        while True:
            sys.stdin.readline()
            self.pressed.set()

    def poll(self) -> bool:
        if not self.pressed.is_set():
            return False
        self.pressed.clear()
        return True


def _check(host: str, text: str, name: str = "") -> Message:
    query = f"?name={urllib.parse.quote(name)}" if name else ""
    status, body = http_call(host, "POST", f"/logo/check{query}", text)
    if status != 200:
        raise SystemExit(f"check: HTTP {status} {body}")
    reply: Message = json.loads(body)
    return reply


def _source(args: argparse.Namespace) -> tuple[str, str]:
    if args.example:
        status, body = http_call(args.host, "GET", f"/logo/programs/{args.example}")
        if status != 200:
            raise SystemExit(f"load {args.example}: HTTP {status} {body}")
        return body, args.example
    if not args.file:
        raise SystemExit("give a program file or --example NAME")
    with open(args.file, encoding="utf-8") as f:
        return f.read(), ""


def _run(args: argparse.Namespace) -> int:
    text, name = _source(args)
    reply = _check(args.host, text, name)
    for line in check_lines(reply):
        print(line)
    if args.cmd == "check" or not reply.get("ok"):
        return 0 if reply.get("ok") else 1
    ws = WsClient(args.host)
    try:
        prog = int(reply["prog"])
        if args.step:
            print("step mode: Enter = next line")
        ended = run_program(ws, prog, args.step, print, EnterKey().poll if args.step else None)
    except KeyboardInterrupt:
        ws.send({"t": "stop"})
        ws.send({"t": "logo_stop", "id": 3})
        print("STOP sent")
        return 1
    finally:
        ws.close()
    return 0 if ended is not None and ended.get("reason") == "finished" else 1


def _store(args: argparse.Namespace) -> int:
    if args.cmd == "list":
        status, body = http_call(args.host, "GET", "/logo/programs")
        programs: list[dict[str, Any]] = (
            json.loads(body).get("programs", []) if status == 200 else []
        )
        for p in programs:
            print(f"{p['name']:24} {p['bytes']:5} B{'  (example)' if p['example'] else ''}")
        return 0 if status == 200 else 1
    if args.cmd == "load":
        status, body = http_call(args.host, "GET", f"/logo/programs/{args.name}")
    elif args.cmd == "save":
        with open(args.file, encoding="utf-8") as f:
            status, body = http_call(args.host, "PUT", f"/logo/programs/{args.name}", f.read())
    else:
        status, body = http_call(args.host, "DELETE", f"/logo/programs/{args.name}")
    print(body)
    return 0 if status == 200 else 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Tank Logo from the laptop (Plan N3)")
    ap.add_argument("cmd", choices=["check", "run", "list", "load", "save", "delete"])
    ap.add_argument(
        "name", nargs="?", help="check / run: a file; load / save / delete: a saved name"
    )
    ap.add_argument("file", nargs="?", help="save: the program file")
    ap.add_argument("--example", default="", help="check / run a saved program or example by name")
    ap.add_argument("--step", action="store_true", help="run: stop at each line (Enter = next)")
    ap.add_argument("--host", default="conqueror-bridge.local")
    args = ap.parse_args(argv)
    if args.cmd in ("check", "run"):
        args.file = args.name
        return _run(args)
    if args.cmd != "list" and not args.name:
        ap.error("give a program name")
    return _store(args)


if __name__ == "__main__":
    sys.exit(main())
