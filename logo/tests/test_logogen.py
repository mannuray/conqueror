import json
from pathlib import Path

import pytest

from logogen import emit, model

ROOT = Path(__file__).resolve().parents[2]


def spec() -> model.Spec:
    return model.load(ROOT)


def test_words_load_and_forward_has_its_range() -> None:
    s = spec()
    fd = s.word("FORWARD")
    assert fd.level == 1 and fd.kind == "command" and fd.needs == ["encoders"]
    assert fd.inputs[0].min_milli == 1000 and fd.inputs[0].max_milli == 500000
    assert s.alias_target("FD") == "FORWARD"


def test_presets_are_strings_and_off_survives_yaml() -> None:
    assert "OFF" in spec().presets["colour"]


def test_normalize_reads() -> None:
    assert (
        emit.normalize_reads("(REPEAT 4 [ (FD 50)\n (RT 90) ])") == "(REPEAT 4 [(FD 50) (RT 90)])"
    )
    assert emit.normalize_reads("(TO SIDE ( ) [ (OUTPUT 50) ])") == "(TO SIDE () [(OUTPUT 50)])"


def test_c_string_escapes_quotes_newlines_and_utf8() -> None:
    assert emit.c_string('a"b\\c\nd') == '"a\\"b\\\\c\\nd"'
    assert emit.c_string("—") == '"\\342\\200\\224"'


def test_cases_skip_v11_and_carry_expectations() -> None:
    cases = spec().cases
    names = [c.name for c in cases]
    assert "a template with two values" not in names  # version v1.1
    fd600 = next(c for c in cases if c.name == "a range known before running")
    assert (fd600.code, fd600.line, fd600.col) == (7025, 1, 4)
    sq = next(c for c in cases if c.name.startswith("the spec's first program"))
    assert sq.your_words == "SQUARE:1:0;"


def test_words_h_mentions_every_word_and_error() -> None:
    s = spec()
    text = emit.render_words_h(s)
    for w in s.words:
        assert f'"{w.name}"' in text
    assert "7029" in text and "kCases" not in text


def test_ifelse_inputs_are_yes_and_no_not_booleans() -> None:
    ins = spec().word("IFELSE").inputs
    assert [i.name for i in ins] == ["test", "yes", "no"]
    assert ins[1].what == "steps in [ ]"


def test_colour_rgb_and_look_deg_follow_the_presets() -> None:
    s = spec()
    text = emit.render_words_h(s)
    assert "inline constexpr Rgb kColourRgb[] = {{255, 0, 0}," in text
    assert "{0, 0, true}" in text  # LOOK HOME
    assert "kRunTimeS = 300" in text and "kNesting = 100" in text


def test_a_colour_without_rgb_is_refused() -> None:
    s = spec()
    del s.colour_rgb["RED"]
    with pytest.raises(ValueError, match="colour_rgb: RED"):
        model._check_preset_data(s)  # pyright: ignore[reportPrivateUsage]


def test_run_cases_carry_their_world_and_ending() -> None:
    runs = spec().run_cases
    square = next(r for r in runs if r.name == "a square")
    assert square.does.startswith("FORWARD 50 | RIGHT 90") and square.end_code == 0
    blocked = next(r for r in runs if r.name == "an obstacle ends a program without BLOCKED?")
    assert (blocked.cut_short, blocked.end_code, blocked.end_line) == (1, 7024, 1)
    text = emit.render_running_h(spec())
    assert "#define LOGO_RUN_CASES(X) X(0)" in text


def test_world_motion_numbers_are_checked() -> None:
    with pytest.raises(ValueError, match="motion 0"):
        model._bits([0])  # pyright: ignore[reportPrivateUsage]


def test_every_word_has_a_colour_group() -> None:
    s = spec()
    assert all(w.group in model.GROUPS for w in s.words)
    groups = {
        n: s.word(n).group for n in ("FORWARD", "RIGHT", "WAIT", "LED", "DISTANCE", "AND", "FIRST")
    }
    assert groups == {
        "FORWARD": "move",
        "RIGHT": "turn",
        "WAIT": "flow",
        "LED": "show",
        "DISTANCE": "sense",
        "AND": "maths",
        "FIRST": "maths",
    }


def test_words_json_carries_the_group() -> None:
    data = json.loads(emit.render_words_json(spec()))
    fd = next(w for w in data["words"] if w["name"] == "FORWARD")
    assert fd["group"] == "move"


def test_a_word_without_a_group_is_refused() -> None:
    with pytest.raises(ValueError, match="FD2: unknown group"):
        model._load_words([{"name": "FD2", "kind": "command", "level": 1}])  # pyright: ignore[reportPrivateUsage]
