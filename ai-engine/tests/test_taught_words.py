"""Taught words beside the measured ones, and one choice of model + vocabulary for every CLI
(AI engine LLD §22.4)."""

from __future__ import annotations

from pathlib import Path

import yaml

from conqueror_ai.adapters.taught_words import MODEL_NAME, WORDS_TXT, TaughtWords, pick_words


def measured(tmp: Path) -> Path:
    p = tmp / "vocabulary.yaml"
    p.write_text(yaml.safe_dump({"words": [{"word": "toy", "floor": True, "also": ["toy car"]}]}))
    return p


def model(folder: Path) -> Path:
    m = folder / MODEL_NAME
    m.mkdir(parents=True)
    return m


def test_add_remove_round_trip(tmp_path: Path) -> None:
    t = TaughtWords(tmp_path / "taught")
    assert t.words() == []
    t.add("cup", ["mug"], "2026-11-04")
    t.add("sock", [], "2026-11-04")
    assert [(w.word, w.names, w.floor, w.measured) for w in t.words()] == [
        ("cup", ("mug",), True, False),
        ("sock", (), True, False),
    ]
    raw = yaml.safe_load(t.vocab_file.read_text())
    assert raw["words"][0] == {
        "word": "cup",
        "floor": True,
        "names": ["mug"],
        "measured": False,
        "taught": "2026-11-04",
    }
    t.remove("cup")
    assert [w.word for w in t.words()] == ["sock"]
    t.remove("nothing")  # not there: nothing happens


def test_pick_taught_when_words_match(tmp_path: Path) -> None:
    cfg, models, taught = measured(tmp_path), tmp_path / "models", tmp_path / "taught"
    model(models)
    TaughtWords(taught).add("cup", ["mug"], "2026-11-04")
    m = model(taught)
    (taught / WORDS_TXT).write_text("toy\ntoy car\ncup\n")
    path, vocab = pick_words(cfg, models, taught)
    assert path == m
    assert vocab.prompts == ["toy", "toy car", "cup"]
    assert [w.measured for w in vocab.words] == [True, False]


def test_pick_measured_when_words_txt_stale(tmp_path: Path) -> None:
    cfg, models, taught = measured(tmp_path), tmp_path / "models", tmp_path / "taught"
    m = model(models)
    TaughtWords(taught).add("cup", [], "2026-11-04")
    model(taught)
    (taught / WORDS_TXT).write_text("toy\ntoy car\n")  # made before cup: not this list
    path, vocab = pick_words(cfg, models, taught)
    assert path == m and vocab.prompts == ["toy", "toy car"]


def test_pick_measured_when_no_taught_model(tmp_path: Path) -> None:
    cfg, models, taught = measured(tmp_path), tmp_path / "models", tmp_path / "taught"
    m = model(models)
    TaughtWords(taught).add("cup", [], "2026-11-04")
    (taught / WORDS_TXT).write_text("toy\ntoy car\ncup\n")
    path, vocab = pick_words(cfg, models, taught)
    assert path == m and vocab.prompts == ["toy", "toy car"]


def test_no_taught_folder_is_the_measured_model(tmp_path: Path) -> None:
    cfg, models = measured(tmp_path), tmp_path / "models"
    m = model(models)
    path, vocab = pick_words(cfg, models, tmp_path / "nothing")
    assert path == m and [w.word for w in vocab.words] == ["toy"]


def test_taught_words_not_measured(tmp_path: Path) -> None:
    t = TaughtWords(tmp_path)
    t.add("cup", [], "2026-11-04")
    assert all(not w.measured for w in t.words())
