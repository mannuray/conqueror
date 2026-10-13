from html.parser import HTMLParser
from pathlib import Path

from protogen.cli import OUTPUTS
from protogen.emit_help import EXAMPLES, emit_help, example_values
from protogen.encoding import build_frame
from protogen.loader import load

ROOT = Path(__file__).resolve().parents[3]
MODEL = load(ROOT / "protocol" / "defs")


def test_registered_as_output() -> None:
    assert OUTPUTS["firmware/esp32/src/generated/help_protocol.html"] is emit_help


def test_examples_are_the_encoder_bytes() -> None:
    html = emit_help(MODEL)
    for name, seq, raw in EXAMPLES:
        data = build_frame(MODEL.meta, MODEL.frame(name), seq, example_values(MODEL, raw))
        assert f"<code>{data.hex(' ')}</code>" in html, name


def test_golden_stop_matches() -> None:
    html = emit_help(MODEL)
    assert "<code>aa 01 12 04 01" in html  # STOP src=HUMAN, same layout as golden.yaml


def test_hello_uses_current_protocol_version() -> None:
    v = MODEL.meta.protocol_version
    assert f"BODY-LINK PROTOCOL v{v}" in emit_help(MODEL)
    assert example_values(MODEL, {"proto": -1}) == {"proto": v}


def test_lists_every_ack_status_and_error() -> None:
    html = emit_help(MODEL)
    for i, reason in enumerate(MODEL.ack_status):
        assert f"{i} {reason}" in html
    for d in MODEL.errors:
        for e in d.codes:
            assert f"{d.prefix}_{e.code}" in html


def test_fragment_is_well_formed() -> None:
    class Checker(HTMLParser):
        def __init__(self) -> None:
            super().__init__()
            self.stack: list[str] = []

        def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
            self.stack.append(tag)

        def handle_endtag(self, tag: str) -> None:
            assert self.stack and self.stack[-1] == tag, (self.stack, tag)
            self.stack.pop()

    checker = Checker()
    checker.feed(emit_help(MODEL))
    assert checker.stack == []
