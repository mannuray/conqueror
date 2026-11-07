"""Words taught from the /ai page (AI engine LLD §22.4), kept apart from the measured ones:
ai-engine/taught/ (git-ignored) holds vocabulary.yaml (each word not measured), the detector model made with
them and words.txt, the exact list that model was made with. One choice of model + vocabulary for every CLI:
the taught model only when its words.txt matches the measured and taught words together, else the measured
model - so the model and its vocabulary always agree (a worktree whose models/ points at the main checkout
never has it overwritten)."""

from __future__ import annotations

from collections.abc import Sequence
from pathlib import Path
from typing import Any, cast

import yaml

from conqueror_ai.adapters.detector_config import load_vocabulary
from conqueror_ai.domain.vocabulary import Vocabulary, Word

MODEL_NAME = "yoloworld-s-640.mlpackage"
WORDS_TXT = "words.txt"
VOCAB_NAME = "vocabulary.yaml"


class TaughtWords:
    def __init__(self, folder: Path) -> None:
        self.folder = folder

    @property
    def vocab_file(self) -> Path:
        return self.folder / VOCAB_NAME

    def _rows(self) -> list[dict[str, Any]]:
        if not self.vocab_file.exists():
            return []
        d: Any = yaml.safe_load(self.vocab_file.read_text()) or {}
        return list(cast("dict[str, Any]", d).get("words") or [])

    def _write(self, rows: list[dict[str, Any]]) -> None:
        self.folder.mkdir(parents=True, exist_ok=True)
        self.vocab_file.write_text(yaml.safe_dump({"words": rows}, sort_keys=False, allow_unicode=True))

    def words(self) -> list[Word]:
        rows = self._rows()
        return list(Vocabulary.from_dict({"words": rows}).words) if rows else []

    def add(self, word: str, names: Sequence[str], today: str) -> None:
        row = {"word": word, "floor": True, "names": list(names), "measured": False, "taught": today}
        self._write([r for r in self._rows() if r.get("word") != word] + [row])

    def remove(self, word: str) -> None:
        rows = self._rows()
        kept = [r for r in rows if r.get("word") != word]
        if len(kept) != len(rows):
            self._write(kept)


def pick_words(config_vocab: Path, models_dir: Path, taught_dir: Path) -> tuple[Path, Vocabulary]:
    """The detector model and the vocabulary it was made with."""
    measured = load_vocabulary(config_vocab)
    measured_pair = (models_dir / MODEL_NAME, measured)
    taught = TaughtWords(taught_dir).words()
    model, listed = taught_dir / MODEL_NAME, taught_dir / WORDS_TXT
    if not taught or not model.exists() or not listed.exists():
        return measured_pair
    merged = Vocabulary.of(measured.words + tuple(taught))
    made_with = [line.strip() for line in listed.read_text().splitlines() if line.strip()]
    return (model, merged) if made_with == merged.prompts else measured_pair
