"""The loose names a child may write for the AI words (AI engine LLD §21.4, Tank Logo LLD §24): the words of
config/vocabulary.yaml, their `names:`, and the colour balls' `names:` (config/balls.yaml) for the word
"ball". `person` is never offered and never resolves. Within the bridge's bounds (bridge LLD R26)."""

from __future__ import annotations

from collections.abc import Sequence

from conqueror_ai.domain.vocabulary import Vocabulary

MAX_WORDS = 48
MAX_NAMES = 8
MAX_NAME_BYTES = 24
NEVER = "person"
BALL = "ball"


def squeeze(name: str) -> str:
    return " ".join(name.lower().split())


class Names:
    def __init__(self, vocab: Vocabulary, balls: Sequence[str]) -> None:
        """`balls`: the names of the word "ball" (balls.yaml `names:`)."""
        rows: dict[str, list[str]] = {}
        for w in vocab.words:
            rows.setdefault(squeeze(w.word), []).extend(squeeze(n) for n in w.names)
        rows.setdefault(BALL, []).extend(squeeze(n) for n in balls)
        rows.pop(NEVER, None)  # the person and its names are not offered
        self._by_name: dict[str, str] = {}
        self._rows: list[tuple[str, list[str]]] = []
        if len(rows) > MAX_WORDS:
            raise ValueError(f"there are {len(rows)} words; the tank takes at most {MAX_WORDS}")
        for word, names in rows.items():
            if not word or "" in names:
                raise ValueError(f"{word or 'a word'} has a blank name")
            names = [n for n in dict.fromkeys(names) if n != word]
            if len(names) > MAX_NAMES:
                raise ValueError(f"{word} has {len(names)} names; at most {MAX_NAMES} names a word")
            for n in (word, *names):
                if len(n.encode()) > MAX_NAME_BYTES:
                    raise ValueError(f"the name '{n}' is longer than {MAX_NAME_BYTES} bytes")
            self._rows.append((word, names))
            for n in (word, *names):
                if n in self._by_name:
                    raise ValueError(f"'{n}' is in the names more than once")
                self._by_name[n] = word
        for w in vocab.words:  # a person's names never resolve, even when another word says them
            if squeeze(w.word) == NEVER:
                for n in w.names:
                    if squeeze(n) in self._by_name:
                        raise ValueError(f"'{squeeze(n)}' is in the names more than once")

    def resolve(self, name: str) -> str | None:
        return self._by_name.get(squeeze(name))

    def wire(self) -> list[dict[str, object]]:
        return [{"word": w, "names": list(n)} for w, n in self._rows]
