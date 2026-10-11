"""protogen command line: write | check."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Callable
from pathlib import Path

from .emit_cpp import emit_cpp
from .emit_docs import emit_docs
from .emit_python import emit_python
from .emit_vectors import emit_vectors_cpp, emit_vectors_json
from .loader import load
from .model import ProtocolModel
from .validator import validate

OUTPUTS: dict[str, Callable[[ProtocolModel], str]] = {
    "protocol/python/conqueror_protocol/generated.py": emit_python,
    "firmware/uno/src/generated/cq_protocol.h": emit_cpp,
    "protocol/vectors/frames.json": emit_vectors_json,
    "protocol/tests/cpp/generated_vectors_test.cpp": emit_vectors_cpp,
    "docs/protocol/reference.md": emit_docs,
}

DEFAULT_ROOT = Path(__file__).resolve().parents[3]


def _load_valid(root: Path) -> ProtocolModel | None:
    model = load(root / "protocol" / "defs")
    issues = validate(model)
    for issue in issues:
        print(f"protogen: {issue}", file=sys.stderr)
    return None if issues else model


def render_all(model: ProtocolModel) -> dict[str, str]:
    return {rel: emit(model) for rel, emit in OUTPUTS.items()}


def cmd_write(root: Path) -> int:
    model = _load_valid(root)
    if model is None:
        return 1
    for rel, text in render_all(model).items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    print(f"protogen: wrote {len(OUTPUTS)} files")
    return 0


def cmd_check(root: Path) -> int:
    model = _load_valid(root)
    if model is None:
        return 1
    stale = [
        rel
        for rel, text in render_all(model).items()
        if not (root / rel).exists() or (root / rel).read_text(encoding="utf-8") != text
    ]
    for rel in stale:
        print(f"protogen: stale or missing {rel} (run: uv run protogen write)", file=sys.stderr)
    return 1 if stale else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="protogen")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("write", help="regenerate all outputs")
    sub.add_parser("check", help="fail if outputs are stale or defs invalid")
    args = parser.parse_args(argv)
    root: Path = args.root
    if args.cmd == "write":
        return cmd_write(root)
    return cmd_check(root)


if __name__ == "__main__":
    raise SystemExit(main())
