"""Phase 0: talking to the bridge - the camera stream and the WebSocket. Read-only except `camera_set`.

The bridge has no authentication (home network, development only). Nothing here moves the tank.
"""

from __future__ import annotations

import http.client
import json
import threading
import time
from collections.abc import Iterator
from typing import Any

from websockets.sync.client import ClientConnection, connect

from measure_lib import MjpegParser

DEFAULT_HOST = "192.168.31.167"  # the tank on the home network (mDNS does not resolve from Python here)


def frames(host: str, port: int = 81) -> Iterator[tuple[float, bytes]]:
    """(laptop receive time, JPEG) for every picture of the camera stream, newest as they come."""
    conn = http.client.HTTPConnection(host, port, timeout=5)
    conn.request("GET", "/stream")
    resp = conn.getresponse()
    if resp.status != 200:
        raise RuntimeError(f"stream answered {resp.status}")
    parser = MjpegParser()
    while True:
        chunk = resp.read1(65536)
        if not chunk:
            return
        now = time.monotonic()
        for jpeg in parser.feed(chunk):
            yield now, jpeg


class Bridge:
    """A WebSocket viewer: says hello, keeps pinging (the bridge drops a silent client after 3 s) and
    keeps the newest `td` sample. It never takes the drive."""

    def __init__(self, host: str) -> None:
        self.ws: ClientConnection = connect(f"ws://{host}/ws", open_timeout=5)
        self.td: dict[str, Any] | None = None
        self.replies: list[dict[str, Any]] = []
        self._lock = threading.Lock()
        self._stop = False
        self.send({"t": "hello", "client": "measure", "proto": 1})
        threading.Thread(target=self._reader, daemon=True).start()
        threading.Thread(target=self._pinger, daemon=True).start()

    def send(self, msg: dict[str, Any]) -> None:
        self.ws.send(json.dumps(msg))

    def debug(self, hz: int = 50) -> None:
        self.send({"t": "debug", "on": True, "hz": hz})

    def camera_size(self, size: str) -> None:
        """QVGA / VGA / SVGA / HD - the bridge keeps it (Settings -> Tank -> Camera shows it too)."""
        self.send({"t": "camera_set", "size": size, "id": 1})

    def latest_td(self) -> dict[str, Any] | None:
        with self._lock:
            return self.td

    def close(self) -> None:
        self._stop = True
        try:
            self.send({"t": "debug", "on": False})
        finally:
            self.ws.close()

    def _reader(self) -> None:
        for raw in self.ws:
            msg = json.loads(raw)
            with self._lock:
                if msg.get("t") == "td":
                    self.td = msg
                elif msg.get("t") in ("ack", "nack", "tank"):
                    self.replies.append(msg)

    def _pinger(self) -> None:
        while not self._stop:
            try:
                self.send({"t": "ping", "t0": int(time.time() * 1000)})
            except Exception:
                return
            time.sleep(1.0)
