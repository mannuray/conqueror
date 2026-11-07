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


def test_world_answers_and_skill_ends_are_carried_and_checked() -> None:
    wait = next(r for r in spec().run_cases if r.name.startswith("WAIT UNTIL SEE?"))
    assert (wait.answers, wait.skill_ends) == ("0 0 1", "done")
    assert model._answers([True, None, 120]) == "1 N 120"  # pyright: ignore[reportPrivateUsage]
    with pytest.raises(ValueError, match="answer"):
        model._answers(["yes"])  # pyright: ignore[reportPrivateUsage]
    with pytest.raises(ValueError, match="skill end"):
        model._skill_ends(["gone"])  # pyright: ignore[reportPrivateUsage]


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


AI_WORDS: dict[str, tuple[str, list[str], list[str]]] = {
    "FIND": ("command", ["thing"], ["ai", "encoders"]),
    "WANDER": ("command", ["number"], ["ai", "encoders"]),
    "EXPLORE": ("command", [], ["ai", "encoders"]),
    "SETXY": ("command", ["number", "number"], ["ai", "encoders"]),
    "AREA": ("command", ["number"], ["ai"]),
    "SEE?": ("reporter", ["thing"], ["ai"]),
    "HOWFAR": ("reporter", ["thing"], ["ai"]),
}


def test_ai_words_are_level_2() -> None:
    s = spec()
    for name, (kind, types, needs) in AI_WORDS.items():
        w = s.word(name)
        assert (w.level, w.kind) == (2, kind), name
        assert [i.type for i in w.inputs] == types, name
        assert sorted(w.needs) == needs, name
        assert w.help, name
        assert name in w.example.split(), name  # the example uses the word itself
    ranges = {
        n: [(i.min_milli, i.max_milli) for i in s.word(n).inputs]
        for n in ("WANDER", "SETXY", "AREA")
    }
    assert ranges == {
        "WANDER": [(1000, 600000)],
        "SETXY": [(-1000000, 1000000)] * 2,
        "AREA": [(0, 10000)],
    }
    assert all(s.word(n).group == "move" for n in ("FIND", "WANDER", "EXPLORE", "SETXY", "AREA"))
    assert all(s.word(n).group == "sense" for n in ("SEE?", "HOWFAR"))
    assert s.word("FOLLOW").level == 3 and s.word("HOWMANY").level == 3
    assert s.word("DISTANCE").level == 2


def test_new_errors_verbatim() -> None:
    e = spec().errors
    assert e[7020] == "{word} needs the AI helper (it runs on the laptop)."
    assert e[7050] == "I couldn't find the {thing}."
    assert e[7051] == "I lost sight of the {thing}."
    assert e[7052] == "The AI helper stopped answering."
    assert e[7053] == "I don't know what a {thing} is."
    assert e[7054] == "I saw the {thing}, but it's outside the play area."
    assert e[7055] == "I can't find a way there."


def test_thing_input_type_is_emitted() -> None:
    text = emit.render_words_h(spec())
    assert "kThing" in text.split("enum class WordKind")[0]
    assert "InType::kThing" in text


def test_tank_things_key() -> None:
    s = spec()
    case = next(c for c in s.cases if c.things)
    assert case.ai is True
    assert case.things == "toy=car|racing car;ball=ball;"
    text = emit.render_cases_h(s)
    assert '"toy=car|racing car;ball=ball;"' in text
    assert "const char* things" in text and "bool ai" in text
    plain = next(c for c in s.cases if c.name == "missing input")
    assert plain.ai is False and plain.things == ""


def test_error_7056_verbatim() -> None:
    assert spec().errors[7056] == "I'm learning a new word - try again in a minute."


def test_tank_learning_key() -> None:
    s = spec()
    case = next(c for c in s.cases if c.learning)
    assert case.ai is True
    text = emit.render_cases_h(s)
    assert "bool learning" in text
    plain = next(c for c in s.cases if c.name == "missing input")
    assert plain.learning is False


def test_skill_end_learning_allowed() -> None:
    run = next(r for r in spec().run_cases if "learning" in r.skill_ends)
    assert run.name == "FIND while the helper learns a word"


@pytest.mark.parametrize(
    "things",
    [
        {"to=y": ["car"]},
        {"toy": ["a|b"]},
        {"toy": ["a;b"]},
        {"to;y": ["car"]},
        {"toy": "car"},
        {"toy": [1]},
        {"toy": None},
        ["toy"],
    ],
)
def test_bad_things_are_refused(things: object) -> None:
    with pytest.raises((TypeError, ValueError), match="things"):
        model._things(things)  # pyright: ignore[reportPrivateUsage]


def test_good_things_still_encode() -> None:
    encoded = model._things({"toy": ["car", "racing car"], "ball": []})  # pyright: ignore[reportPrivateUsage]
    assert encoded == "toy=car|racing car;ball=;"
