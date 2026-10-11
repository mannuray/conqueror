from pathlib import Path

from protogen.diff import breaking_changes
from protogen.loader import load
from protogen.model import Field

DEFS = Path(__file__).resolve().parents[2] / "defs"


def test_identical_is_not_breaking() -> None:
    assert breaking_changes(load(DEFS), load(DEFS)) == []


def test_removed_frame_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frames = [f for f in new.frames if f.name != "CAM_HOME"]
    assert any("CAM_HOME" in c and "removed" in c for c in breaking_changes(old, new))


def test_changed_field_type_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields[2].type = "i32"
    assert any("TURN" in c for c in breaking_changes(old, new))


def test_appended_field_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("STOP").fields.append(Field(name="extra", type="u8"))
    assert any("STOP" in c for c in breaking_changes(old, new))


def test_doc_change_is_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields[0].doc = "changed wording"
    assert breaking_changes(old, new) == []


def test_new_frame_is_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    old.frames = [f for f in old.frames if f.name != "CAM_HOME"]
    assert breaking_changes(old, new) == []
