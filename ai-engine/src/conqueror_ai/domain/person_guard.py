"""Never a box over a person (AI engine LLD §21.4, design-notes §197): the detector always asks for `person`,
and a non-person box that sits on a person box is the person, not a thing. Dropped before the tracker, so a
person is never chased, checked as a candidate or remembered, whatever word was asked."""

from __future__ import annotations

from collections.abc import Sequence

from conqueror_ai.domain.types import Detection

OVERLAP = 0.3  # of the box's own area
# Words for people, refused when teaching a word (design-notes §203) - a second check: the real rule is the
# guard below, with the detector reporting a person on its own score whatever word wins its box.
PERSON_WORDS = frozenset(
    [
        "person",
        "people",
        "human",
        "humans",
        "kid",
        "kids",
        "child",
        "children",
        "baby",
        "babies",
        "toddler",
        "man",
        "men",
        "woman",
        "women",
        "boy",
        "boys",
        "girl",
        "girls",
        "mum",
        "mummy",
        "mom",
        "mommy",
        "mother",
        "dad",
        "daddy",
        "father",
        "brother",
        "sister",
        "grandma",
        "grandpa",
        "granny",
        "me",
    ]
)


def _inside(a: tuple[float, float, float, float], b: tuple[float, float, float, float]) -> float:
    """The share of box a's own area that lies inside box b."""
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    area = (a[2] - a[0]) * (a[3] - a[1])
    if w <= 0 or h <= 0 or area <= 0:
        return 0.0
    return w * h / area


def clear(dets: Sequence[Detection]) -> list[Detection]:
    people = [d.box for d in dets if d.cls == "person"]
    return [d for d in dets if d.cls == "person" or all(_inside(d.box, p) < OVERLAP for p in people)]
