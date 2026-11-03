"""Ports the app drives through (AI engine LLD §19.5, review F9): `DriveLink` behind `Driver` for cq-drive;
Plan Z2 adds a lease driver for the Tank Logo skill server."""

from __future__ import annotations

import asyncio
from typing import Protocol, runtime_checkable

from conqueror_ai.domain.drive.curves import Command


@runtime_checkable
class Driver(Protocol):
    @property
    def connected(self) -> bool: ...  # the link is up

    @property
    def refused(self) -> str | None: ...  # why the tank refused a command (the run ends), or None

    async def run(self, stop: asyncio.Event) -> None: ...

    async def drive(self, cmd: Command) -> None: ...

    async def cam(self, pan_deg: float, tilt_deg: float) -> None: ...

    async def finish(self) -> None: ...
