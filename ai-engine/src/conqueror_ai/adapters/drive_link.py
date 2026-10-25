"""The only part of the engine that moves the tank (AI engine LLD §17.7). `BridgeLink` stays motion-free (§6).

Its own WebSocket ("ai-drive"):
- `drive_rate` (speed % and turn rate, gyro-held on the bridge, R4.4), refreshed at least every 0.2 s, so
  the bridge's 0.3 s deadman fires only when the engine stalls;
- `cam_set` (as the driver it may move the camera);
- `release_control` at the end.
A refused drive_rate (a nack without an id: STOPPED - someone pressed STOP -, NOT_OWNER, INVALID_COMMAND
...) is kept in `refused`, and the app ends the run; a refused camera move ends it only when the tank isn't
ours."""

from __future__ import annotations

import asyncio
import itertools
import json
import time
from typing import Any, cast

from websockets.asyncio.client import ClientConnection, connect

from conqueror_ai.domain.drive.curves import STOP, Command

# cm/s -> drive_rate v % (design-notes §69-§71: the Uno's dead-band lift makes it non-linear); measured
# again in the floor checks and kept in config/drive.yaml
SPEED_TABLE: tuple[tuple[float, float], ...] = ((0.0, 0.0), (17.7, 15.0), (22.0, 40.0), (33.5, 100.0))
REFRESH_S = 0.2
FRESH_FOR_S = 0.5  # the refresh repeats a command only this long: a stuck engine is left to the deadman
CAM_ENDS = ("NOT_OWNER", "STOPPED")  # a refused camera move ends the run only for these (not BUSY)


def to_percent(v_cms: float, table: tuple[tuple[float, float], ...] = SPEED_TABLE) -> int:
    """The drive_rate speed for a speed in cm/s, linear between the table's points, 0..100."""
    if v_cms <= 0:
        return 0
    for (v0, p0), (v1, p1) in itertools.pairwise(table):
        if v_cms <= v1:
            return round(p0 + (p1 - p0) * (v_cms - v0) / (v1 - v0))
    return 100


def drive_msg(cmd: Command, table: tuple[tuple[float, float], ...] = SPEED_TABLE) -> dict[str, Any]:
    return {"t": "drive_rate", "v": to_percent(cmd.v_cms, table), "yaw_rate": round(cmd.w_dps, 1)}


class DriveLink:
    def __init__(
        self, host: str, port: int = 80, table: tuple[tuple[float, float], ...] = SPEED_TABLE
    ) -> None:
        self.url = f"ws://{host}:{port}/ws"
        self.table = table
        self.refused: str | None = None  # the bridge refused driving: the run must end
        self.cmd = STOP
        self._ws: ClientConnection | None = None
        self._next_id = 0
        self._sent_s = 0.0
        self._set_s = 0.0  # when the engine last gave a command
        self._drove = False  # a drive command was sent: the end must stop and hand control back

    @property
    def connected(self) -> bool:
        return self._ws is not None

    async def run(self, stop: asyncio.Event) -> None:
        async with connect(self.url, ping_interval=None, open_timeout=5) as ws:
            self._ws = ws
            await ws.send(json.dumps({"t": "hello", "client": "ai-drive", "proto": 1}))
            keep = asyncio.create_task(self._keep(ws, stop))
            try:
                async for raw in ws:
                    if stop.is_set():
                        break
                    self._on_text(raw if isinstance(raw, str) else raw.decode("utf-8", "replace"))
            finally:
                keep.cancel()
                self._ws = None

    def _on_text(self, raw: str) -> None:
        try:
            m: Any = json.loads(raw)
        except ValueError:
            return
        if not isinstance(m, dict):
            return
        msg = cast("dict[str, Any]", m)
        if msg.get("t") == "nack":
            # no id: a stream message refused - the drive_rate (any reason, INVALID_COMMAND too); with an
            # id: a camera move, which ends the run only when the tank is no longer ours
            reason = str(msg.get("reason") or "refused")
            if msg.get("id") is None or reason in CAM_ENDS:
                self.refused = self.refused or reason

    async def _keep(self, ws: ClientConnection, stop: asyncio.Event) -> None:
        """Pings (the bridge drops a client silent for 3 s) and the drive refresh (the 0.3 s deadman). The
        refresh stops FRESH_FOR_S after the engine's last command, so a stuck engine stops the tank."""
        last_ping = 0.0
        while not stop.is_set():
            now = time.monotonic()
            if now - last_ping >= 1.0:
                await ws.send(json.dumps({"t": "ping", "t0": round(now * 1000, 3)}))
                last_ping = now
            fresh = now - self._set_s <= FRESH_FOR_S
            if self.cmd != STOP and fresh and now - self._sent_s >= REFRESH_S:
                await self._send_drive(ws)
            await asyncio.sleep(0.05)

    async def _send_drive(self, ws: ClientConnection) -> None:
        await ws.send(json.dumps(drive_msg(self.cmd, self.table)))
        self._sent_s = time.monotonic()

    async def drive(self, cmd: Command) -> None:
        """The new command, sent at once; then refreshed every 0.2 s until it changes."""
        ws = self._ws
        if ws is None:
            raise ConnectionError("not connected to the bridge")
        moving_before = self.cmd != STOP
        self.cmd = cmd
        self._set_s = time.monotonic()
        if cmd != STOP or moving_before:
            self._drove = True
            await self._send_drive(ws)

    async def cam(self, pan_deg: float, tilt_deg: float) -> None:
        """The camera (pan + = right, tilt + = up). Not waited for: a refused one is only noted."""
        ws = self._ws
        if ws is None:
            return
        self._next_id += 1
        await ws.send(
            json.dumps(
                {"t": "cam_set", "pan": round(pan_deg, 1), "tilt": round(tilt_deg, 1), "id": self._next_id}
            )
        )

    async def finish(self) -> None:
        """Stop, then hand control back - only if it drove: a stop from a run that never drove would hold a
        phone driver's tank until they let go of the stick (bridge R9.6)."""
        ws = self._ws
        self.cmd = STOP
        if ws is None or not self._drove:
            return
        await ws.send(json.dumps({"t": "stop"}))
        await ws.send(json.dumps({"t": "release_control"}))
