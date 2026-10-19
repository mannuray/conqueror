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
