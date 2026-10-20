"""HTTP chunked transfer encoding, removed as bytes arrive (the bridge's esp_http_server sends every stream part
as chunks: `<hex length>\\r\\n<data>\\r\\n`, ending with `0\\r\\n\\r\\n`). Pure."""

from __future__ import annotations

import re

_HEX = re.compile(rb"[0-9a-fA-F]+")


class Dechunker:
    def __init__(self) -> None:
        self._buf = bytearray()
        self._left = 0  # data bytes still to come in the current chunk
        self._crlf = False  # the CRLF after a chunk's data is still to come
        self.done = False

    def feed(self, data: bytes) -> bytes:
        """The body bytes in `data` (and in what was kept from earlier feeds). A bad length line -> ValueError."""
        self._buf += data
        out = bytearray()
        while not self.done:
            if self._left > 0:
                take = min(self._left, len(self._buf))
                if take == 0:
                    break
                out += self._buf[:take]
                del self._buf[:take]
                self._left -= take
                if self._left == 0:
                    self._crlf = True
                continue
            if self._crlf:
                if len(self._buf) < 2:
                    break
                if self._buf[:2] != b"\r\n":
                    raise ValueError("no CRLF after a chunk")
                del self._buf[:2]
                self._crlf = False
                continue
            end = self._buf.find(b"\r\n")
            if end < 0:
                if len(self._buf) > 1024:
                    raise ValueError("chunk length line too long")
                break
            size = bytes(self._buf[:end]).split(b";", 1)[0].strip()  # a chunk extension is ignored
            if not _HEX.fullmatch(size):
                raise ValueError(f"bad chunk length {size!r}")
            del self._buf[: end + 2]
            self._left = int(size, 16)
            if self._left == 0:
                self.done = True  # the last chunk (trailers are not used by the bridge)
        return bytes(out)
