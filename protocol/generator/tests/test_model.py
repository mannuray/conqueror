import pytest
from pydantic import ValidationError

from protogen.model import Field, Frame, ItemField


def test_scalar_and_array_sizes() -> None:
    frame = Frame(
        name="X",
        type=0x10,
        dir="esp32_to_uno",
        fields=[
            Field(name="a", type="u8"),
            Field(name="b", type="i32"),
            Field(name="c", type="i16", count=8),
        ],
    )
    assert frame.max_payload() == 1 + 4 + 16
    assert frame.fixed_size() == 21


def test_struct_array_sizes() -> None:
    frame = Frame(
        name="TONE",
        type=0x32,
        dir="esp32_to_uno",
        fields=[
            Field(name="n", type="u8"),
            Field(
                name="notes",
                items=[ItemField(name="hz", type="u16"), ItemField(name="ms", type="u16")],
                count_field="n",
                max_count=10,
            ),
        ],
    )
    assert frame.max_payload() == 1 + 40
    assert frame.fixed_size() == 1
    assert frame.fields[1].is_struct_array


def test_field_needs_type_or_items() -> None:
    with pytest.raises(ValidationError):
        Field(name="bad")


def test_struct_array_needs_count_field() -> None:
    with pytest.raises(ValidationError):
        Field(name="bad", items=[ItemField(name="x", type="u8")], max_count=3)


def test_unknown_key_is_rejected() -> None:
    with pytest.raises(ValidationError):
        Field.model_validate({"name": "a", "type": "u8", "untis": "ms"})
