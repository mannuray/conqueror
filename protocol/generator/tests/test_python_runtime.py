from collections.abc import Sequence
from pathlib import Path
from typing import cast

import pytest

from conqueror_protocol import FrameParser, RawFrame, build_frame, decode_frame, generated
from conqueror_protocol.framing import crc8
from protogen.loader import load
from protogen.naming import pascal
from protogen.vectors import Vector, make_vectors

DEFS = Path(__file__).resolve().parents[2] / "defs"
MODEL = load(DEFS)


def _norm(v: object) -> object:
    if isinstance(v, (list, tuple)):
        return [_norm(x) for x in cast(Sequence[object], v)]
    return v


@pytest.mark.parametrize("vec", make_vectors(MODEL), ids=lambda v: f"{v.frame}-{v.variant}")
def test_runtime_matches_vector(vec: Vector) -> None:
    frame_def = MODEL.frame(vec.frame)
    parser = FrameParser()
    [raw] = parser.feed(vec.data)
    msg = decode_frame(raw, frame_def.dir)
    assert type(msg).__name__ == pascal(vec.frame)
    for name, expected in vec.values.items():
        assert _norm(getattr(msg, name)) == _norm(expected), name
    assert build_frame(msg, raw.seq) == vec.data


def test_crc_check_value() -> None:
    assert crc8(b"123456789") == 0xF4


def test_parser_split_and_noise() -> None:
    data = bytes.fromhex(MODEL.golden[2].hex.replace(" ", ""))
    parser = FrameParser()
    frames: list[RawFrame] = []
    for b in b"\x00\x13\xff" + data:
        frames += parser.feed(bytes([b]))
    assert len(frames) == 1
    assert frames[0].type == 0x13


def test_parser_bad_crc_then_good() -> None:
    good = bytes.fromhex(MODEL.golden[1].hex.replace(" ", ""))
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    parser = FrameParser()
    frames = parser.feed(bytes(bad) + good)
    assert parser.stats.bad_crc == 1
    assert [f.type for f in frames] == [0x12]


def test_parser_bad_len() -> None:
    parser = FrameParser()
    assert parser.feed(bytes([0xAA, 49, 0x10, 0])) == []
    assert parser.stats.bad_len == 1


def test_tone_rejects_bad_count() -> None:
    with pytest.raises(ValueError):
        generated.Tone.decode(bytes([11]) + bytes(44))
    with pytest.raises(ValueError):
        generated.Tone.decode(bytes([2]) + bytes(4))
    with pytest.raises(ValueError):  # n disagrees with the number of notes
        generated.Tone(n=3, notes=((440, 100),)).encode()
    with pytest.raises(ValueError):  # more notes than max_count
        generated.Tone(n=11, notes=((440, 100),) * 11).encode()


def test_short_and_long_payloads_rejected() -> None:
    with pytest.raises(ValueError):
        generated.Turn.decode(bytes(6))
    with pytest.raises(ValueError):
        generated.Turn.decode(bytes(8))


def test_registry_and_tables() -> None:
    assert generated.REGISTRY[("esp32_to_uno", 0x13)] is generated.Turn
    assert generated.ACK_STATUS[1] == "NOT_OWNER"
    assert generated.ERRORS["NOT_OWNER"].code == "COMMAND_3001"
    assert generated.SettingId.DEADMAN == 15
    assert generated.BAUD == 250000
