"""The bridge's camera stream (multipart MJPEG, Plan Q1 headers) -> pictures with their capture time. Pure."""

from __future__ import annotations

from collections.abc import Iterator
from dataclasses import dataclass

BOUNDARY = b"--cqframe"


@dataclass(frozen=True)
class Part:
    seq: int
    cap_us: int
    jpeg: bytes


class MjpegParser:
    """Feed bytes as they come; yields every complete part. Parts without `X-Frame-Us` (an older bridge)
    are skipped: the engine cannot place them in time."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, chunk: bytes) -> Iterator[Part]:
        self._buf += chunk
        while True:
            start = self._buf.find(BOUNDARY)
            if start < 0:
                del self._buf[: max(0, len(self._buf) - len(BOUNDARY))]  # keep a possible half boundary
                return
            head_end = self._buf.find(b"\r\n\r\n", start)
            if head_end < 0:
                del self._buf[:start]
                return
            headers = _headers(bytes(self._buf[start + len(BOUNDARY) : head_end]))
            length = headers.get("content-length")
            if length is None or not length.isdigit():
                del self._buf[: head_end + 4]  # a broken header: skip it
                continue
            body_start = head_end + 4
            body_end = body_start + int(length)
            if len(self._buf) < body_end:
                del self._buf[:start]
                return
            jpeg = bytes(self._buf[body_start:body_end])
            del self._buf[:body_end]
            us, seq = headers.get("x-frame-us"), headers.get("x-frame-seq", "0")
            if us is not None and us.isdigit() and seq.isdigit():
                yield Part(int(seq), int(us), jpeg)


def _headers(block: bytes) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in block.decode("latin-1").split("\r\n"):
        name, sep, value = line.partition(":")
        if sep:
            out[name.strip().lower()] = value.strip()
    return out
