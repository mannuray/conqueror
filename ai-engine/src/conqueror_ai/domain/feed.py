"""The AI feed's binary frame for the /ai page (AI engine LLD §22.2): a 4-byte big-endian header length, the
header JSON, the camera JPEG, then the depth JPEG (none: "depth": 0). One message = one moment."""

from __future__ import annotations

import json
import struct
from collections.abc import Sequence
from typing import Any

from conqueror_ai.domain.types import Detection

MAX_BOXES = 8


def page_boxes(dets: Sequence[Detection], w: int, h: int) -> list[list[object]]:
    """Sure boxes only (maybes left out), the highest scores first, at most 8, as [word, score, x, y, w, h] in
    0..1000 of the picture; a colour-finder ball (ball:<name>) shows as "ball"."""
    sure = sorted((d for d in dets if d.sure), key=lambda d: -d.score)[:MAX_BOXES]
    out: list[list[object]] = []
    for d in sure:
        x1, y1, x2, y2 = d.box
        word = "ball" if d.cls.startswith("ball:") else d.cls
        out.append(
            [
                word,
                round(d.score, 2),
                round(x1 * 1000 / w),
                round(y1 * 1000 / h),
                round((x2 - x1) * 1000 / w),
                round((y2 - y1) * 1000 / h),
            ]
        )
    return out


def encode_frame(header: dict[str, Any], cam: bytes, depth: bytes | None) -> bytes:
    head = json.dumps({**header, "cam": len(cam), "depth": len(depth or b"")}, separators=(",", ":")).encode()
    return struct.pack(">I", len(head)) + head + cam + (depth or b"")


def decode_frame(b: bytes) -> tuple[dict[str, Any], bytes, bytes | None]:
    (n,) = struct.unpack(">I", b[:4])
    head: dict[str, Any] = json.loads(b[4 : 4 + n])
    at = 4 + n
    cam = b[at : at + int(head["cam"])]
    d = int(head["depth"])
    depth = b[at + len(cam) : at + len(cam) + d] if d else None
    return head, cam, depth
