"""Names: the loose names a child may write for the AI words (AI engine LLD §21.4)."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path

import pytest

from conqueror_ai.adapters.detector_config import load_ball_names, load_vocabulary, save_ball
from conqueror_ai.domain.balls import BallColour
from conqueror_ai.domain.names import Names
from conqueror_ai.domain.vocabulary import Vocabulary


def vocab(extra: Sequence[dict[str, object]] | None = None) -> Vocabulary:
    rows: list[dict[str, object]] = [
        {"word": "person", "floor": True, "names": ["human"]},
        {"word": "toy", "floor": True, "also": ["toy car"], "names": ["car", "Racing  Car", "toy car"]},
        {"word": "box", "floor": True, "names": ["cardboard box"]},
    ]
    return Vocabulary.from_dict({"words": rows + list(extra or [])})


def test_resolve_word_and_names() -> None:
    n = Names(vocab(), ["football"])
    assert n.resolve("toy") == "toy"
    assert n.resolve("Racing   CAR") == "toy"
    assert n.resolve(" car ") == "toy"
    assert n.resolve("football") == "ball"
    assert n.resolve("ball") == "ball"
    assert n.resolve("dragon") is None
    assert {"word": "toy", "names": ["car", "racing car", "toy car"]} in n.wire()
    assert {"word": "ball", "names": ["football"]} in n.wire()


def test_person_never_offered_or_resolved() -> None:
    n = Names(vocab(), [])
    assert n.resolve("person") is None and n.resolve("human") is None
    assert all(w["word"] != "person" and "human" not in w["names"] for w in n.wire())  # type: ignore[operator]


def test_bounds_refused() -> None:
    many: list[dict[str, object]] = [{"word": f"w{i}", "floor": True} for i in range(48)]
    with pytest.raises(ValueError, match="48"):
        Names(vocab(many), [])
    with pytest.raises(ValueError, match="8 names"):
        Names(vocab([{"word": "cup", "names": [f"n{i}" for i in range(9)]}]), [])
    with pytest.raises(ValueError, match="24"):
        Names(vocab([{"word": "cup", "names": ["x" * 25]}]), [])
    with pytest.raises(ValueError, match="more than once"):
        Names(vocab([{"word": "cup", "names": ["car"]}]), [])


def test_config_files_load_and_keep_ball_names(tmp_path: Path) -> None:
    cfg = Path(__file__).parent.parent / "config"
    n = Names(load_vocabulary(cfg / "vocabulary.yaml"), load_ball_names(cfg / "balls.yaml"))
    assert n.resolve("teddy") == "toy" and n.resolve("football") == "ball" and n.resolve("person") is None
    f = tmp_path / "balls.yaml"
    f.write_text("names: [football]\nballs: []\n")
    save_ball(f, BallColour("red", 0, 10, 80, 255))
    assert load_ball_names(f) == ["football"]


def test_exact_limits_accepted_and_one_over_refused() -> None:
    n = Names(Vocabulary.from_dict({"words": [{"word": f"w{i}"} for i in range(47)]}), [])
    assert len(n.wire()) == 48
    with pytest.raises(ValueError, match="48"):
        Names(Vocabulary.from_dict({"words": [{"word": f"w{i}"} for i in range(48)]}), [])
    eight = [f"n{i}" for i in range(8)]
    Names(vocab([{"word": "cup", "names": eight}]), [])
    with pytest.raises(ValueError, match="8 names"):
        Names(vocab([{"word": "cup", "names": [*eight, "n8"]}]), [])
    assert Names(vocab([{"word": "cup", "names": ["x" * 24]}]), []).resolve("x" * 24) == "cup"
    with pytest.raises(ValueError, match="24"):
        Names(vocab([{"word": "cup", "names": ["é" * 13]}]), [])  # 26 bytes
    with pytest.raises(ValueError, match="24"):
        Names(vocab([{"word": "y" * 25}]), [])  # the word itself


def test_a_blank_name_is_an_error() -> None:
    with pytest.raises(ValueError, match="blank"):
        Names(vocab([{"word": "cup", "names": ["  "]}]), [])
    with pytest.raises(ValueError, match="blank"):
        Names(vocab(), [""])
