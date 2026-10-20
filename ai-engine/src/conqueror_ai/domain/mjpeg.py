"""The bridge's camera stream (multipart MJPEG, Plan Q1 headers) -> pictures with their capture time. Pure."""

from __future__ import annotations

import re
from collections.abc import Iterator
from dataclasses import dataclass

BOUNDARY = b"--cqframe"
MAX_PICTURE = 2_000_000  # bytes; an HD JPEG from the OV3660 is well under 300 KB
_DIGITS = re.compile(r"[0-9]+")


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
            length = _number(headers.get("content-length"))
            if length is None or length > MAX_PICTURE:
                del self._buf[: head_end + 4]  # a broken or nonsense header: look for the next part
                continue
            body_start = head_end + 4
            body_end = body_start + length
            if len(self._buf) < body_end:
                del self._buf[:start]
                return
            jpeg = bytes(self._buf[body_start:body_end])
            del self._buf[:body_end]
            us, seq = _number(headers.get("x-frame-us")), _number(headers.get("x-frame-seq", "0"))
            if us is not None and seq is not None:
                yield Part(seq, us, jpeg)


def _headers(block: bytes) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in block.decode("latin-1").split("\r\n"):
        name, sep, value = line.partition(":")
        if sep:
            out[name.strip().lower()] = value.strip()
    return out


def _number(text: str | None) -> int | None:
    """A whole number written in ASCII digits only (str.isdigit() also accepts e.g. '²')."""
    if text is None or not _DIGITS.fullmatch(text):
        return None
    return int(text)
