"""A stand-in tank for the /logo browser check (Tank Logo LLD §21.9): HTTP only, standard library.

Serves the packed /logo page and answers /logo/check and /logo/programs like the bridge
(LLD §20.4), one request at a time like its logo worker (a second gets 503 busy). The WebSocket
is faked inside the browser by the check itself (an injected WebSocket class), as in Plans M, O.
    python3 firmware/esp32/web/test/fake_tank.py [port]   -> http://127.0.0.1:8765/logo
A check refuses any text containing FORWAD (7001 at that word); a text containing "; bigpath"
gets a 2000-point spiral (the bridge's most) cut short; any other text is OK with a 50 cm square.
"""

from __future__ import annotations

import json
import math
import pathlib
import re
import runpy
import sys
import threading
import time
from collections.abc import Callable
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, cast
from urllib.parse import urlsplit

PROJECT = pathlib.Path(__file__).resolve().parents[2]
_PACK = runpy.run_path(str(PROJECT / "scripts" / "pack_web.py"), run_name="pack_web")
assemble_page = cast(Callable[[pathlib.Path, str], bytes], _PACK["assemble_page"])  # icons in (bridge LLD R25)
gz = cast(Callable[[bytes], bytes], _PACK["gz"])

PROGRAMS: dict[str, str] = {"square": "REPEAT 4 [ FD 50 RT 90 ]\n"}
EXAMPLES = {"first_square": "; a square\nREPEAT 4 [ FORWARD 50 RIGHT 90 ]\n"}
SQUARE = [
    [0, 0, 0, 1, 0],
    [0, 500, 0, 1, 0],
    [0, 500, 90, 1, 0],
    [500, 500, 90, 1, 0],
    [500, 500, 180, 1, 0],
    [500, 0, 180, 1, 0],
    [500, 0, 270, 1, 1],
    [0, 0, 270, 1, 1],
]
state = {"prog": 0, "busy": 0}
WORKER = threading.Lock()


def check(text: str) -> dict[str, Any]:
    state["prog"] += 1
    for n, line in enumerate(text.split("\n"), 1):
        m = re.search(r"FORWAD", line, re.IGNORECASE)
        if m:
            diag = {
                "code": 7001,
                "line": n,
                "col": m.start() + 1,
                "msg": "I don't know FORWAD. Did you mean FORWARD?",
                "warning": False,
            }
            return {
                "ok": False,
                "prog": state["prog"],
                "diags": [diag],
                "words": [],
                "handles_obstacles": False,
                "path": [],
                "cut_short": False,
            }
    path, cut = SQUARE, False
    if "; bigpath" in text:
        path = [
            [round(math.sin(i / 20) * i), round(math.cos(i / 20) * i), i % 360, 1, int(i > 1000)]
            for i in range(2000)
        ]
        cut = True
    words = [
        {"name": w.upper(), "inputs": 0, "gives": False}
        for w in re.findall(r"\bTO\s+(\w+)", text, re.I)
    ]
    return {
        "ok": True,
        "prog": state["prog"],
        "diags": [],
        "words": words,
        "handles_obstacles": False,
        "path": path,
        "cut_short": cut,
    }


class Handler(BaseHTTPRequestHandler):
    def reply(self, code: int, body: bytes, ctype: str, gz: bool = False) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def json(self, obj: Any, code: int = 200) -> None:
        self.reply(code, json.dumps(obj).encode(), "application/json")

    def body(self) -> str:
        return self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode("utf-8")

    def where(self) -> str:
        return urlsplit(self.path).path

    def name(self) -> str:
        return self.where().rsplit("/", 1)[1]

    def job(self, work: Callable[[], None]) -> None:
        """Like the bridge's logo worker: one /logo/* request at a time, a second one gets 503."""
        if not WORKER.acquire(blocking=False):
            self.body()  # the bridge drains the body too
            state["busy"] += 1
            return self.json({"ok": False, "why": "BUSY"}, 503)
        try:
            time.sleep(0.05)  # a check takes ~40 ms on the bridge
            work()
        finally:
            WORKER.release()

    def do_GET(self) -> None:
        path = self.where()
        if path in ("/", "/settings", "/logo"):  # the packed pages, as the bridge serves them
            return self.reply(200, gz(assemble_page(PROJECT, path)), "text/html; charset=utf-8", True)
        if path == "/fake/stats":  # for the browser check: how many requests were refused busy
            return self.json({"busy": state["busy"]})
        if path == "/logo/programs":
            return self.job(self.list)
        if path.startswith("/logo/programs/"):
            return self.job(self.load)
        self.reply(404, b"", "text/plain")

    def list(self) -> None:
        rows = [{"name": n, "bytes": len(t), "example": False} for n, t in sorted(PROGRAMS.items())]
        rows += [{"name": n, "bytes": len(t), "example": True} for n, t in EXAMPLES.items()]
        self.json({"programs": rows})

    def load(self) -> None:
        text = PROGRAMS.get(self.name(), EXAMPLES.get(self.name()))
        if text is None:
            return self.json({"ok": False, "why": "NOT_FOUND"}, 404)
        self.reply(200, text.encode(), "text/plain; charset=utf-8")

    def do_POST(self) -> None:
        if self.where() != "/logo/check":
            return self.reply(404, b"", "text/plain")
        self.job(lambda: self.json(check(self.body())))

    def do_PUT(self) -> None:
        self.job(self.save)

    def save(self) -> None:
        n, text = self.name(), self.body()
        if n in EXAMPLES:
            return self.json({"ok": False, "why": "EXAMPLE"}, 409)
        if not re.fullmatch(r"[a-z0-9_-]{1,24}", n):
            return self.json({"ok": False, "why": "NAME"}, 409)
        PROGRAMS[n] = text
        self.json({"ok": True})

    def do_DELETE(self) -> None:
        self.job(self.remove)

    def remove(self) -> None:
        n = self.name()
        if n in EXAMPLES:
            return self.json({"ok": False, "why": "EXAMPLE"}, 409)
        PROGRAMS.pop(n, None)
        self.json({"ok": True})

    def log_message(self, format: str, *args: Any) -> None:
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    print(f"fake tank: http://127.0.0.1:{port}/logo", flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
