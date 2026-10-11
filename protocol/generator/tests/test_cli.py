import shutil
from pathlib import Path

import pytest

from protogen.cli import OUTPUTS, cmd_check, cmd_write
from protogen.emit_docs import emit_docs
from protogen.loader import load

ROOT = Path(__file__).resolve().parents[3]


def _tmp_root(tmp_path: Path) -> Path:
    shutil.copytree(ROOT / "protocol" / "defs", tmp_path / "protocol" / "defs")
    return tmp_path


def test_write_then_check_clean(tmp_path: Path) -> None:
    root = _tmp_root(tmp_path)
    assert cmd_write(root) == 0
    for rel in OUTPUTS:
        assert (root / rel).exists(), rel
    assert cmd_check(root) == 0


def test_check_detects_stale(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = _tmp_root(tmp_path)
    cmd_write(root)
    capsys.readouterr()
    header = root / "firmware/uno/src/generated/cq_protocol.h"
    header.write_text(header.read_text() + "// edited by hand\n")
    assert cmd_check(root) == 1
    err = capsys.readouterr().err
    assert "firmware/uno/src/generated/cq_protocol.h" in err
    assert "protocol/vectors/frames.json" not in err


def test_check_detects_missing_regeneration(tmp_path: Path) -> None:
    root = _tmp_root(tmp_path)
    cmd_write(root)
    meta = root / "protocol/defs/meta.yaml"
    meta.write_text(meta.read_text().replace("protocol_version: 1", "protocol_version: 2"))
    assert cmd_check(root) == 1


def test_write_refuses_invalid_defs(tmp_path: Path) -> None:
    root = _tmp_root(tmp_path)
    meta = root / "protocol/defs/meta.yaml"
    meta.write_text(meta.read_text().replace("max_payload: 48", "max_payload: 8"))
    assert cmd_write(root) == 1


def test_docs_escape_pipes_in_table_cells() -> None:
    text = emit_docs(load(ROOT / "protocol" / "defs"))
    assert "major << 8 \\| minor" in text  # HELLO.fw doc
    assert "PrimitiveKind \\| ok << 8" in text  # PRIMITIVE_DONE.a


def test_committed_outputs_are_fresh() -> None:
    assert cmd_check(ROOT) == 0
