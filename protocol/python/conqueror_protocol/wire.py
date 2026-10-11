"""Hand-written runtime used by the generated message classes."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING, Any, ClassVar, Self

FMT: dict[str, str] = {"u8": "B", "i8": "b", "u16": "H", "i16": "h", "u32": "I", "i32": "i"}


@dataclass(frozen=True, slots=True)
class FieldSpec:
    name: str
    wire: str | None
    count: int | None = None
    items: tuple[tuple[str, str], ...] | None = None
    count_field: str | None = None
    max_count: int | None = None


@dataclass(frozen=True, slots=True)
class ErrorInfo:
    number: int
    code: str
    reason: str
    http: int
    message: str


@dataclass(frozen=True, slots=True)
class SettingInfo:
    id: int
    name: str
    scope: str
    wire: str
    unit: str
    scale: float
    min: int
    max: int
    default: int


class Message:
    """Base for generated frame dataclasses."""

    __slots__ = ()
    TYPE: ClassVar[int]
    DIR: ClassVar[str]
    ACK: ClassVar[bool]
    STREAMING: ClassVar[bool]
    FIELDS: ClassVar[tuple[FieldSpec, ...]]

    if TYPE_CHECKING:

        def __init__(self, **kwargs: Any) -> None: ...

    def encode(self) -> bytes:
        out = bytearray()
        for spec in self.FIELDS:
            value: Any = getattr(self, spec.name)
            if spec.items is not None:
                assert spec.max_count is not None and spec.count_field is not None
                count = getattr(self, spec.count_field)
                if len(value) > spec.max_count or count != len(value):
                    raise ValueError(
                        f"{type(self).__name__}.{spec.name}: {len(value)} items, "
                        f"{spec.count_field}={count}, max {spec.max_count}"
                    )
                fmt = "<" + "".join(FMT[w] for _, w in spec.items)
                for item in value:
                    out += struct.pack(fmt, *item)
            elif spec.count is not None:
                assert spec.wire is not None
                out += struct.pack(f"<{spec.count}{FMT[spec.wire]}", *value)
            else:
                assert spec.wire is not None
                out += struct.pack("<" + FMT[spec.wire], value)
        return bytes(out)

    @classmethod
    def decode(cls, payload: bytes) -> Self:
        values: dict[str, Any] = {}
        pos = 0
        try:
            for spec in cls.FIELDS:
                if spec.items is not None:
                    assert spec.count_field is not None and spec.max_count is not None
                    n = int(values[spec.count_field])
                    if n > spec.max_count:
                        raise ValueError(f"{cls.__name__}.{spec.name}: {n} > {spec.max_count}")
                    fmt = "<" + "".join(FMT[w] for _, w in spec.items)
                    size = struct.calcsize(fmt)
                    items: list[tuple[int, ...]] = []
                    for _ in range(n):
                        items.append(struct.unpack_from(fmt, payload, pos))
                        pos += size
                    values[spec.name] = tuple(items)
                elif spec.count is not None:
                    assert spec.wire is not None
                    fmt = f"<{spec.count}{FMT[spec.wire]}"
                    values[spec.name] = struct.unpack_from(fmt, payload, pos)
                    pos += struct.calcsize(fmt)
                else:
                    assert spec.wire is not None
                    fmt = "<" + FMT[spec.wire]
                    (values[spec.name],) = struct.unpack_from(fmt, payload, pos)
                    pos += struct.calcsize(fmt)
        except struct.error as exc:
            raise ValueError(f"{cls.__name__}: payload too short") from exc
        if pos != len(payload):
            raise ValueError(f"{cls.__name__}: {len(payload) - pos} unexpected trailing bytes")
        return cls(**values)
