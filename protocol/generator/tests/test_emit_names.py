from pathlib import Path

from protogen.cli import OUTPUTS
from protogen.emit_names import emit_names
from protogen.loader import load

ROOT = Path(__file__).resolve().parents[3]
MODEL = load(ROOT / "protocol" / "defs")


def test_registered_as_output() -> None:
    assert OUTPUTS["firmware/esp32/src/generated/cq_protocol_names.h"] is emit_names


def test_every_event_named_at_its_kind() -> None:
    text = emit_names(MODEL)
    for ev in MODEL.events:
        assert f'/* {ev.kind} */ "{ev.name.lower()}"' in text


def test_gaps_are_unknown_and_ack_status_named() -> None:
    text = emit_names(MODEL)
    assert '/* 0 */ "unknown"' in text  # kind 0 is not an event
    for i, status in enumerate(MODEL.ack_status):
        assert f'/* {i} */ "{status.lower()}"' in text
