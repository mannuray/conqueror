from pathlib import Path

from protogen.loader import load

DEFS = Path(__file__).resolve().parents[2] / "defs"


def test_loads_real_defs() -> None:
    model = load(DEFS)
    assert model.meta.baud == 250000
    assert model.meta.max_payload == 48
    names = {f.name for f in model.frames}
    assert {"HEARTBEAT", "TURN", "TONE", "TELEM_FAST", "TELEM_SLOW", "CAL_DATA"} <= names
    busy = next(e for d in model.errors for e in d.codes if e.reason == "BUSY")
    assert busy.message == "Busy, try again"  # comma inside a flow mapping must be quoted


def test_payload_sizes_match_lld() -> None:
    model = load(DEFS)
    assert model.frame("TELEM_FAST").max_payload() == 40
    assert model.frame("TELEM_SLOW").max_payload() == 36
    assert model.frame("CAL_DATA").max_payload() == 20
    assert model.frame("TONE").max_payload() == 41
    assert model.frame("TURN").max_payload() == 7


def test_ack_status_starts_with_ok() -> None:
    model = load(DEFS)
    assert model.ack_status[0] == "OK"


def test_yaml_boolean_words_stay_strings() -> None:
    model = load(DEFS)  # YAML 1.1 turns bare on/off/yes/no into booleans; the defs quote them
    assert model.frame("CAM_LOCK").fields[0].name == "on"
    assert "OFF" in next(e for e in model.enums if e.name == "LedMode").values
