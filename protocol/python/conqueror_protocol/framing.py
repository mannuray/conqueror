"""Frame building and streaming parsing: 0xAA | len | type | seq | payload | crc8."""

from __future__ import annotations

from dataclasses import dataclass

from .generated import MAX_PAYLOAD, REGISTRY, SYNC
from .wire import Message


class UnknownFrameError(ValueError):
    pass


def crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def build_frame(msg: Message, seq: int) -> bytes:
    payload = msg.encode()
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} > {MAX_PAYLOAD}")
    head = bytes([len(payload), msg.TYPE, seq & 0xFF])
    return bytes([SYNC]) + head + payload + bytes([crc8(head + payload)])


@dataclass(frozen=True, slots=True)
class RawFrame:
    type: int
    seq: int
    payload: bytes


@dataclass(slots=True)
class ParserStats:
    frames: int = 0
    bad_crc: int = 0
    bad_len: int = 0


class FrameParser:
    """Byte-at-a-time state machine (same states as the Uno parser)."""

    def __init__(self) -> None:
        self._state = "sync"
        self._len = 0
        self._type = 0
        self._seq = 0
        self._buf = bytearray()
        self.stats = ParserStats()

    def feed(self, data: bytes) -> list[RawFrame]:
        frames: list[RawFrame] = []
        for byte in data:
            frame = self._feed_byte(byte)
            if frame is not None:
                frames.append(frame)
        return frames

    def _feed_byte(self, b: int) -> RawFrame | None:
        if self._state == "sync":
            if b == SYNC:
                self._state = "len"
            return None
        if self._state == "len":
            if b > MAX_PAYLOAD:
                self.stats.bad_len += 1
                self._state = "sync"
                return None
            self._len = b
            self._state = "type"
            return None
        if self._state == "type":
            self._type = b
            self._state = "seq"
            return None
        if self._state == "seq":
            self._seq = b
            self._buf.clear()
            self._state = "payload" if self._len else "crc"
            return None
        if self._state == "payload":
            self._buf.append(b)
            if len(self._buf) == self._len:
                self._state = "crc"
            return None
        self._state = "sync"
        expected = crc8(bytes([self._len, self._type, self._seq]) + bytes(self._buf))
        if b != expected:
            self.stats.bad_crc += 1
            return None
        self.stats.frames += 1
        return RawFrame(self._type, self._seq, bytes(self._buf))


def decode_frame(raw: RawFrame, direction: str) -> Message:
    cls = REGISTRY.get((direction, raw.type))
    if cls is None:
        raise UnknownFrameError(f"unknown {direction} frame type 0x{raw.type:02X}")
    return cls.decode(raw.payload)
