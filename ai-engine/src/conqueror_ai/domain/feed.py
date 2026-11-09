"""The AI feed's binary frame for the /ai page (AI engine LLD §22.2): a 4-byte big-endian header length, the
header JSON, the camera JPEG, then the depth JPEG (none: "depth": 0). One message = one moment."""

from __future__ import annotations

import json
import struct
import zlib
from collections.abc import Sequence
from typing import Any

import numpy as np
from numpy.typing import NDArray

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


def encode_frame(
    header: dict[str, Any], cam: bytes, depth: bytes | None, marks: NDArray[np.uint8] | None = None
) -> bytes:
    """marks: the picture's class picture (LLD §26.2), sent after the depth zlib-packed, row by row (raw it
    is 19 200 B a VGA frame - final review I1); the header says its size and its packed length (`marks:
    [w, h, n]`). A reader that does not know it reads cam and depth as before."""
    head_d: dict[str, Any] = {**header, "cam": len(cam), "depth": len(depth or b"")}
    tail = b""
    if marks is not None:
        tail = zlib.compress(np.ascontiguousarray(marks, dtype=np.uint8).tobytes(), 1)
        head_d["marks"] = [int(marks.shape[1]), int(marks.shape[0]), len(tail)]
    head = json.dumps(head_d, separators=(",", ":")).encode()
    return struct.pack(">I", len(head)) + head + cam + (depth or b"") + tail


def decode_frame(b: bytes) -> tuple[dict[str, Any], bytes, bytes | None]:
    (n,) = struct.unpack(">I", b[:4])
    head: dict[str, Any] = json.loads(b[4 : 4 + n])
    at = 4 + n
    cam = b[at : at + int(head["cam"])]
    d = int(head["depth"])
    depth = b[at + len(cam) : at + len(cam) + d] if d else None
    return head, cam, depth


def decode_marks(b: bytes) -> tuple[int, int, bytes] | None:
    """A frame's class picture: (w, h, its bytes row by row), or None when it has none (LLD §26.2)."""
    (n,) = struct.unpack(">I", b[:4])
    head: dict[str, Any] = json.loads(b[4 : 4 + n])
    if "marks" not in head:
        return None
    w, h, packed = (int(v) for v in head["marks"])
    at = 4 + n + int(head["cam"]) + int(head["depth"])
    return w, h, zlib.decompress(b[at : at + packed])


def encode_map(header: dict[str, Any], png: bytes) -> bytes:
    """The ROOM tab's map frame (LLD §23.2): the same layout, the header with kind map and the PNG's size."""
    head = json.dumps({**header, "kind": "map", "png": len(png)}, separators=(",", ":")).encode()
    return struct.pack(">I", len(head)) + head + png


def decode_map(b: bytes) -> tuple[dict[str, Any], bytes]:
    (n,) = struct.unpack(">I", b[:4])
    head: dict[str, Any] = json.loads(b[4 : 4 + n])
    return head, b[4 + n : 4 + n + int(head["png"])]
