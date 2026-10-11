"""Generator-side encoder (independent of the runtime codecs it cross-checks)."""

from __future__ import annotations

import struct
from collections.abc import Mapping, Sequence
from typing import Any, cast

from .model import Frame, Meta

FMT: dict[str, str] = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i"}


def crc8(data: bytes, poly: int = 0x07, init: int = 0x00) -> int:
    crc = init
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def encode_payload(frame: Frame, values: Mapping[str, Any]) -> bytes:
    out = bytearray()
    for f in frame.fields:
        value = values[f.name]
        if f.items is not None:
            fmt = "<" + "".join(FMT[i.type] for i in f.items)
            for item in cast(Sequence[Sequence[int]], value):
                out += struct.pack(fmt, *item)
        elif f.count is not None:
            assert f.type is not None
            out += struct.pack(f"<{f.count}{FMT[f.type]}", *cast(Sequence[int], value))
        else:
            assert f.type is not None
            out += struct.pack("<" + FMT[f.type], cast(int, value))
    return bytes(out)


def build_frame(meta: Meta, frame: Frame, seq: int, values: Mapping[str, Any]) -> bytes:
    payload = encode_payload(frame, values)
    head = bytes([len(payload), frame.type, seq & 0xFF])
    crc = crc8(head + payload, meta.crc_poly, meta.crc_init)
    return bytes([meta.sync]) + head + payload + bytes([crc])
