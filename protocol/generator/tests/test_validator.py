from pathlib import Path

from protogen.loader import load
from protogen.model import ErrorDef, Field, ProtocolModel, SettingDef
from protogen.validator import validate

DEFS = Path(__file__).resolve().parents[2] / "defs"


def _model() -> ProtocolModel:
    return load(DEFS)


def test_real_defs_are_valid() -> None:
    assert validate(_model()) == []


def test_duplicate_frame_type() -> None:
    m = _model()
    m.frame("STOP").type = m.frame("DRIVE").type
    assert any("duplicate type" in i for i in validate(m))


def test_direction_range() -> None:
    m = _model()
    m.frame("HELLO").type = 0x05
    assert any("HELLO" in i and "range" in i for i in validate(m))


def test_payload_too_big() -> None:
    m = _model()
    m.frame("STOP").fields.append(Field(name="pad", type="i32", count=12))
    assert any("payload" in i and "STOP" in i for i in validate(m))


def test_error_code_outside_domain() -> None:
    m = _model()
    m.errors[0].codes.append(ErrorDef(code=2500, reason="X_BAD", http=400, message="x"))
    assert any("2500" in i for i in validate(m))


def test_retired_code_reused() -> None:
    m = _model()
    m.errors[0].retired.append(1001)
    assert any("1001" in i and "retired" in i for i in validate(m))


def test_ack_status_unknown_reason() -> None:
    m = _model()
    m.ack_status.append("NO_SUCH_REASON")
    assert any("NO_SUCH_REASON" in i for i in validate(m))


def test_setting_default_out_of_range() -> None:
    m = _model()
    m.settings.append(
        SettingDef(
            id=99, name="bad", scope="global", type="u8", unit="x", min=0, max=10, default=11
        )
    )
    assert any("bad" in i and "default" in i for i in validate(m))


def test_setting_range_exceeds_wire_type() -> None:
    m = _model()
    m.settings.append(
        SettingDef(
            id=98, name="wide", scope="global", type="u8", unit="x", min=0, max=300, default=1
        )
    )
    assert any("wide" in i and "wire" in i for i in validate(m))


def test_enum_name_must_not_clash_with_frame_class() -> None:
    m = _model()
    m.enums[0].name = "AudioMode"
    assert any("AudioMode" in i and "clash" in i for i in validate(m))


def test_count_field_must_precede_array() -> None:
    m = _model()
    tone = m.frame("TONE")
    tone.fields.reverse()
    assert any("TONE" in i and "count_field" in i for i in validate(m))
