from pathlib import Path

from protogen.encoding import build_frame, crc8
from protogen.loader import load
from protogen.vectors import make_vectors

DEFS = Path(__file__).resolve().parents[2] / "defs"


def test_crc8_check_value() -> None:
    assert crc8(b"123456789") == 0xF4


def test_golden_vectors_match_generator_encoder() -> None:
    model = load(DEFS)
    for g in model.golden:
        data = build_frame(model.meta, model.frame(g.frame), g.seq, g.values)
        assert data.hex(" ") == g.hex, g.frame


def test_vectors_cover_every_frame_twice() -> None:
    model = load(DEFS)
    vectors = make_vectors(model)
    for frame in model.frames:
        variants = {v.variant for v in vectors if v.frame == frame.name}
        assert {"typical", "extremes"} <= variants, frame.name


def test_extremes_hit_wire_limits() -> None:
    model = load(DEFS)
    vec = next(v for v in make_vectors(model) if v.frame == "MOVE" and v.variant == "extremes")
    assert vec.values["dist_mm"] == -0x80000000
    assert vec.values["src"] == 0xFF


def test_tone_extremes_use_max_count() -> None:
    model = load(DEFS)
    vec = next(v for v in make_vectors(model) if v.frame == "TONE" and v.variant == "extremes")
    assert vec.values["n"] == 10
    assert len(vec.values["notes"]) == 10
