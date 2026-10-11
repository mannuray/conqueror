import shutil
import subprocess
from pathlib import Path

import pytest

from protogen.emit_cpp import emit_cpp
from protogen.emit_vectors import emit_vectors_cpp
from protogen.loader import load

ROOT = Path(__file__).resolve().parents[3]
DEFS = ROOT / "protocol" / "defs"
CXX = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")


@pytest.mark.skipif(CXX is None, reason="no C++ compiler")
def test_cpp_codec_matches_all_vectors(tmp_path: Path) -> None:
    model = load(DEFS)
    inc = tmp_path / "inc"
    inc.mkdir()
    (inc / "cq_protocol.h").write_text(emit_cpp(model))
    src = tmp_path / "vectors_test.cpp"
    src.write_text(emit_vectors_cpp(model))
    exe = tmp_path / "vectors_test"
    assert CXX is not None
    subprocess.run(
        [
            CXX,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pedantic",
            f"-I{inc}",
            str(src),
            "-o",
            str(exe),
        ],
        check=True,
    )
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert ", 0 failed" in result.stdout


@pytest.mark.skipif(CXX is None, reason="no C++ compiler")
def test_cpp_tone_rejects_overlong(tmp_path: Path) -> None:
    model = load(DEFS)
    (tmp_path / "cq_protocol.h").write_text(emit_cpp(model))
    src = tmp_path / "tone.cpp"
    src.write_text(
        '#include "cq_protocol.h"\n'
        "using namespace cq::proto;\n"
        "int main() {\n"
        "  uint8_t bad_count[1 + 44] = {11};\n"
        "  ToneMsg m{};\n"
        "  if (decode(bad_count, sizeof bad_count, m)) { return 1; }\n"
        "  uint8_t bad_len[1 + 4] = {2};\n"
        "  if (decode(bad_len, sizeof bad_len, m)) { return 2; }\n"
        "  m.n = 11;\n"
        "  uint8_t out[kMaxPayload];\n"
        "  if (encode(m, out) != kEncodeError) { return 3; }\n"
        "  return 0;\n"
        "}\n"
    )
    exe = tmp_path / "tone"
    assert CXX is not None
    subprocess.run(
        [
            CXX,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{tmp_path}",
            str(src),
            "-o",
            str(exe),
        ],
        check=True,
    )
    assert subprocess.run([str(exe)]).returncode == 0
