"""The words YOLO-World looks for (AI engine LLD §16.1 item 2): config/vocabulary.yaml as a pure value."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, cast

DEFAULT_MIN_SCORE = 0.25


@dataclass(frozen=True)
class Word:
    word: str
    floor: bool  # it stands on the floor: the floor trick places it
    min_score: float = DEFAULT_MIN_SCORE
    also: tuple[str, ...] = ()  # extra wordings the model is asked, reported as this word (design-notes §187)
    names: tuple[str, ...] = ()  # what a child may write for it in the AI words (LLD §21.4)


def _word(r: dict[str, Any], i: int) -> Word:
    if "word" not in r:
        raise ValueError(f"word row {i} has no word")
    word = str(r["word"])
    floor = r.get("floor", True)
    if not isinstance(floor, bool):
        raise ValueError(f"{word}: floor must be true or false, not {floor!r}")
    also: Any = r.get("also", [])
    if not isinstance(also, list):
        raise ValueError(f"{word}: also must be a list of wordings, not {also!r}")
    names: Any = r.get("names", [])
    if not isinstance(names, list):
        raise ValueError(f"{word}: names must be a list of names, not {names!r}")
    try:
        return Word(
            word,
            floor,
            float(r.get("min_score", DEFAULT_MIN_SCORE)),
            tuple(str(a) for a in cast("list[Any]", also)),
            tuple(str(a) for a in cast("list[Any]", names)),
        )
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
        names = [p for w in words for p in (w.word, *w.also)]
        dup = sorted({n for n in names if names.count(n) > 1})
        if dup:
            raise ValueError(f"the vocabulary has {', '.join(dup)} more than once")
        return Vocabulary(words)

    @property
    def names(self) -> list[str]:
        return [w.word for w in self.words]

    @property
    def prompts(self) -> list[str]:
        """What the model is asked, in its columns' order: each word, then its extra wordings."""
        return [p for w in self.words for p in (w.word, *w.also)]

    @property
    def name_of(self) -> list[str]:
        """The word each of the model's columns reports as."""
        return [w.word for w in self.words for _ in (w.word, *w.also)]

    @property
    def floor_classes(self) -> frozenset[str]:
        return frozenset(w.word for w in self.words if w.floor)

    def min_score(self, word: str) -> float:
        for w in self.words:
            if w.word == word:
                return w.min_score
        return DEFAULT_MIN_SCORE

    def check_model(self, model_names: list[str]) -> None:
        """A model exported for other words is refused: its baked-in names must be the wordings, in order."""
        if model_names != self.prompts:
            raise ValueError(
                "the detector model was made for other words "
                f"({', '.join(model_names)}) - re-export: cd tools/measure && uv run python export_world.py"
            )
