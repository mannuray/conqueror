"""Pydantic model of protocol/defs.

Every model forbids unknown keys, so a YAML typo (or an unquoted comma in a flow mapping)
fails loudly instead of being silently dropped.
"""

from __future__ import annotations

from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, model_validator
from pydantic import Field as PField

WireType = Literal["u8", "i8", "u16", "i16", "u32", "i32"]
Direction = Literal["esp32_to_uno", "uno_to_esp32"]

WIRE_SIZE: dict[str, int] = {"u8": 1, "i8": 1, "u16": 2, "i16": 2, "u32": 4, "i32": 4}
WIRE_RANGE: dict[str, tuple[int, int]] = {
    "u8": (0, 0xFF),
    "i8": (-0x80, 0x7F),
    "u16": (0, 0xFFFF),
    "i16": (-0x8000, 0x7FFF),
    "u32": (0, 0xFFFFFFFF),
    "i32": (-0x80000000, 0x7FFFFFFF),
}


class ItemField(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str
    type: WireType
    unit: str | None = None


class Field(BaseModel):
    """A payload field: scalar, fixed array (`count`) or struct array (`items`)."""

    model_config = ConfigDict(extra="forbid")

    name: str
    type: WireType | None = None
    count: int | None = None
    items: list[ItemField] | None = None
    count_field: str | None = None
    max_count: int | None = None
    unit: str | None = None
    scale: float | None = None
    range: tuple[float, float] | None = None
    enum: str | None = None
    sentinel: dict[int, str] | None = None
    doc: str | None = None

    @model_validator(mode="after")
    def _shape(self) -> Field:
        if self.items is None and self.type is None:
            raise ValueError(f"field {self.name}: needs 'type' or 'items'")
        if self.items is not None and (self.count_field is None or self.max_count is None):
            raise ValueError(f"field {self.name}: struct array needs count_field and max_count")
        return self

    @property
    def is_struct_array(self) -> bool:
        return self.items is not None

    def item_size(self) -> int:
        assert self.items is not None
        return sum(WIRE_SIZE[i.type] for i in self.items)

    def max_size(self) -> int:
        if self.items is not None:
            assert self.max_count is not None
            return self.max_count * self.item_size()
        assert self.type is not None
        return WIRE_SIZE[self.type] * (self.count or 1)


class Frame(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str
    type: int
    dir: Direction
    ack: bool = False
    streaming: bool = False
    doc: str = ""
    fields: list[Field] = PField(default_factory=list[Field])

    def max_payload(self) -> int:
        return sum(f.max_size() for f in self.fields)

    def fixed_size(self) -> int:
        return sum(f.max_size() for f in self.fields if not f.is_struct_array)


class EnumDef(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str
    values: dict[str, int]
    doc: str = ""


class ErrorDef(BaseModel):
    model_config = ConfigDict(extra="forbid")

    code: int
    reason: str
    http: int
    message: str


class ErrorDomain(BaseModel):
    model_config = ConfigDict(extra="forbid")

    prefix: str
    range: tuple[int, int]
    codes: list[ErrorDef] = PField(default_factory=list[ErrorDef])
    retired: list[int] = PField(default_factory=list[int])


class EventDef(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str
    kind: int
    a: str | None = None
    b: str | None = None
    doc: str = ""


class SettingDef(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: int
    name: str
    scope: Literal["global", "profile"]
    type: WireType
    unit: str
    scale: float = 1.0
    min: int
    max: int
    default: int
    doc: str = ""


class GoldenVector(BaseModel):
    model_config = ConfigDict(extra="forbid")

    frame: str
    seq: int
    values: dict[str, Any]
    hex: str


class Meta(BaseModel):
    model_config = ConfigDict(extra="forbid")

    protocol_version: int
    sync: int
    max_payload: int
    crc_poly: int
    crc_init: int
    baud: int


class ProtocolModel(BaseModel):
    model_config = ConfigDict(extra="forbid")

    meta: Meta
    enums: list[EnumDef]
    frames: list[Frame]
    errors: list[ErrorDomain]
    ack_status: list[str]
    events: list[EventDef]
    settings: list[SettingDef]
    golden: list[GoldenVector]

    def frame(self, name: str) -> Frame:
        for f in self.frames:
            if f.name == name:
                return f
        raise KeyError(name)
