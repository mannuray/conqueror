import re
import shutil
import subprocess
from pathlib import Path

import pytest

from protogen.diff import breaking_changes, cmd_diff
from protogen.loader import load
from protogen.model import Field

DEFS = Path(__file__).resolve().parents[2] / "defs"


def _bump_version(meta_text: str) -> str:
    """Return meta.yaml text with protocol_version incremented, whatever its value."""
    match = re.search(r"protocol_version: (\d+)", meta_text)
    assert match is not None
    return meta_text.replace(match.group(0), f"protocol_version: {int(match.group(1)) + 1}")


def test_identical_is_not_breaking() -> None:
    assert breaking_changes(load(DEFS), load(DEFS)) == []


def test_removed_frame_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frames = [f for f in new.frames if f.name != "CAM_HOME"]
    assert any("CAM_HOME" in c and "removed" in c for c in breaking_changes(old, new))


def test_changed_field_type_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields[2].type = "i32"
    assert any(
        "TURN" in c and "fields changed" in c and "i32" in c for c in breaking_changes(old, new)
    )


def test_appended_field_is_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("STOP").fields.append(Field(name="extra", type="u8"))
    assert any(
        "STOP" in c and "fields changed" in c and "extra" in c for c in breaking_changes(old, new)
    )


def test_doc_change_is_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields[0].doc = "changed wording"
    assert breaking_changes(old, new) == []


def test_new_frame_is_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    old.frames = [f for f in old.frames if f.name != "CAM_HOME"]
    assert breaking_changes(old, new) == []


def test_type_id_changed() -> None:
    old, new = load(DEFS), load(DEFS)
    t = old.frame("STOP").type
    new.frame("STOP").type = t + 100
    assert f"frame STOP: type id changed {t} -> {t + 100}" in breaking_changes(old, new)


def test_direction_changed() -> None:
    old, new = load(DEFS), load(DEFS)
    f = new.frame("STOP")
    f.dir = "uno_to_esp32" if f.dir == "esp32_to_uno" else "esp32_to_uno"
    assert any("frame STOP: direction changed" in c for c in breaking_changes(old, new))


def test_ack_flag_changed() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("STOP").ack = not old.frame("STOP").ack
    assert any("frame STOP: ack flag changed" in c for c in breaking_changes(old, new))


def test_field_removed() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields.pop()
    assert any("frame TURN: fields changed" in c for c in breaking_changes(old, new))


def test_field_reordered() -> None:
    old, new = load(DEFS), load(DEFS)
    new.frame("TURN").fields.reverse()
    assert any("frame TURN: fields changed" in c for c in breaking_changes(old, new))


def test_field_scale_changed() -> None:
    old, new = load(DEFS), load(DEFS)
    fld = next(f for f in new.frame("TURN").fields if f.scale is not None)
    assert fld.scale is not None
    fld.scale *= 2
    assert any("frame TURN: fields changed" in c for c in breaking_changes(old, new))


def test_field_range_and_unit_change_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    fld = new.frame("TURN").fields[0]
    fld.unit = "furlongs"
    fld.range = (-1.0, 1.0)
    assert breaking_changes(old, new) == []


def test_ack_status_reordered() -> None:
    old, new = load(DEFS), load(DEFS)
    new.ack_status[0], new.ack_status[1] = new.ack_status[1], new.ack_status[0]
    assert any("ack_status reordered or entries removed" in c for c in breaking_changes(old, new))


def test_ack_status_entry_removed() -> None:
    old, new = load(DEFS), load(DEFS)
    new.ack_status.pop()
    assert any("ack_status reordered or entries removed" in c for c in breaking_changes(old, new))


def test_ack_status_append_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.ack_status.append("NEW_STATUS")
    assert breaking_changes(old, new) == []


def test_error_code_removed() -> None:
    old, new = load(DEFS), load(DEFS)
    new.errors[0].codes.pop(0)
    code = old.errors[0].codes[0]
    assert f"error AUTH code {code.code} ({code.reason}) removed" in breaking_changes(old, new)


def test_error_reason_changed() -> None:
    old, new = load(DEFS), load(DEFS)
    code = old.errors[0].codes[0]
    new.errors[0].codes[0].reason = "OTHER_REASON"
    expected = f"error AUTH code {code.code}: reason changed {code.reason} -> OTHER_REASON"
    assert expected in breaking_changes(old, new)


def test_retired_code_reused() -> None:
    old, new = load(DEFS), load(DEFS)
    code = new.errors[0].codes[0].code
    old.errors[0].retired.append(code)
    new.errors[0].retired.append(code)
    changes = breaking_changes(old, new)
    assert any(f"error code {code}" in c and "reuses a retired code" in c for c in changes)


def test_retired_entry_removed() -> None:
    old, new = load(DEFS), load(DEFS)
    old.errors[0].retired.append(1999)
    assert "error AUTH: retired code 1999 removed from retired list" in breaking_changes(old, new)


def test_enum_value_removed() -> None:
    old, new = load(DEFS), load(DEFS)
    name = next(iter(old.enums[0].values))
    value = old.enums[0].values[name]
    del new.enums[0].values[name]
    expected = f"enum {old.enums[0].name}: value {name}={value} removed"
    assert expected in breaking_changes(old, new)


def test_enum_value_changed() -> None:
    old, new = load(DEFS), load(DEFS)
    name = next(iter(old.enums[0].values))
    new.enums[0].values[name] += 50
    assert any(f"{name} changed" in c for c in breaking_changes(old, new))


def test_enum_value_added_not_breaking() -> None:
    old, new = load(DEFS), load(DEFS)
    new.enums[0].values["BRAND_NEW"] = 250
    assert breaking_changes(old, new) == []


def test_setting_removed() -> None:
    old, new = load(DEFS), load(DEFS)
    gone = new.settings.pop(0)
    assert f"setting {gone.name} removed" in breaking_changes(old, new)


def test_setting_retyped() -> None:
    old, new = load(DEFS), load(DEFS)
    s = old.settings[0]
    new.settings[0].type = "i32" if s.type != "i32" else "u8"
    assert any(f"setting {s.name}: type changed" in c for c in breaking_changes(old, new))


def _git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", *args],
        check=True,
        capture_output=True,
    )


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    shutil.copytree(DEFS, tmp_path / "protocol" / "defs")
    _git(tmp_path, "init", "-q")
    _git(tmp_path, "add", "-A")
    _git(tmp_path, "commit", "-q", "-m", "init")
    return tmp_path


def _break_defs(repo: Path) -> None:
    path = repo / "protocol" / "defs" / "errors.yaml"
    text = path.read_text(encoding="utf-8")
    path.write_text(text.replace("  - CANCELLED\n", ""), encoding="utf-8")
    assert path.read_text(encoding="utf-8") != text


def test_cmd_diff_no_change(repo: Path) -> None:
    assert cmd_diff(repo, "HEAD") == 0


def test_cmd_diff_breaking_without_bump(repo: Path, capsys: pytest.CaptureFixture[str]) -> None:
    _break_defs(repo)
    assert cmd_diff(repo, "HEAD") == 1
    assert "bump protocol_version" in capsys.readouterr().err


def test_cmd_diff_breaking_with_bump(repo: Path) -> None:
    _break_defs(repo)
    meta = repo / "protocol" / "defs" / "meta.yaml"
    meta.write_text(
        _bump_version(meta.read_text(encoding="utf-8")),
        encoding="utf-8",
    )
    assert cmd_diff(repo, "HEAD") == 0


def test_cmd_diff_bad_ref(repo: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert cmd_diff(repo, "no-such-ref") == 2
    assert "protogen: cannot read defs at no-such-ref" in capsys.readouterr().err
