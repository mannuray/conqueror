"""The words YOLO-World looks for (AI engine LLD §16.1 item 2): config/vocabulary.yaml as a pure value."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

DEFAULT_MIN_SCORE = 0.25


@dataclass(frozen=True)
class Word:
    word: str
    floor: bool  # it stands on the floor: the floor trick places it
    min_score: float = DEFAULT_MIN_SCORE


def _word(r: dict[str, Any], i: int) -> Word:
    if "word" not in r:
        raise ValueError(f"word row {i} has no word")
    word = str(r["word"])
    floor = r.get("floor", True)
    if not isinstance(floor, bool):
        raise ValueError(f"{word}: floor must be true or false, not {floor!r}")
    try:
        return Word(word, floor, float(r.get("min_score", DEFAULT_MIN_SCORE)))
    except (TypeError, ValueError):
        raise ValueError(f"{word}: min_score must be a number, not {r.get('min_score')!r}") from None


@dataclass(frozen=True)
class Vocabulary:
    words: tuple[Word, ...]

    @staticmethod
    def from_dict(d: dict[str, Any]) -> Vocabulary:
        """{"words": [{"word": ..., "floor": ..., "min_score": ...}, ...]}; a word twice or no word is
        refused."""
        rows: list[dict[str, Any]] = list(d.get("words") or [])
        if not rows:
            raise ValueError("the vocabulary has no words")
        words = tuple(_word(r, i) for i, r in enumerate(rows, 1))
        names = [w.word for w in words]
        dup = sorted({n for n in names if names.count(n) > 1})
        if dup:
            raise ValueError(f"the vocabulary has {', '.join(dup)} more than once")
        return Vocabulary(words)

    @property
    def names(self) -> list[str]:
        return [w.word for w in self.words]

    @property
    def floor_classes(self) -> frozenset[str]:
        return frozenset(w.word for w in self.words if w.floor)

    def min_score(self, word: str) -> float:
        for w in self.words:
            if w.word == word:
                return w.min_score
        return DEFAULT_MIN_SCORE

    def check_model(self, model_names: list[str]) -> None:
        """A model exported for other words is refused: its baked-in names must be these, in this order."""
        if model_names != self.names:
            raise ValueError(
                "the detector model was made for other words "
                f"({', '.join(model_names)}) - re-export: cd tools/measure && uv run python export_world.py"
            )
