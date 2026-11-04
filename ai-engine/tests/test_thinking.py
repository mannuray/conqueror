"""Thinking: what the tank says on the /ai page, in a child's words (AI engine LLD §22.3)."""

from __future__ import annotations

import pytest

from conqueror_ai.domain.thinking import Thinking, Thought, why_lines

ROWS = [
    (None, None, None, None, Thought("ready", "happy", "Ready to play!")),
    ("find", "start", None, "toy", Thought("looking", "curious", "I'm looking round for the toy.")),
    ("find", "drive", None, "toy", Thought("searching", "curious", "I'm searching for the toy.")),
    ("find", "check", None, "toy", Thought("checking", "puzzled", "Is that the toy? Let me check.")),
    ("find", "chase", None, "toy", Thought("chasing", "excited", "I see the toy! Going to it.")),
    ("find", "recover", None, "toy", Thought("unsticking", "puzzled", "Oops - I'm getting myself unstuck.")),
    ("wander", "drive", None, None, Thought("exploring", "curious", "I'm exploring the room.")),
    ("explore", "start", None, None, Thought("exploring", "curious", "I'm exploring the room.")),
    ("wander", "recover", None, None, Thought("unsticking", "puzzled", "Oops - I'm getting myself unstuck.")),
    ("setxy", "drive", None, None, Thought("going", "busy", "I'm going there.")),
    ("find", "done", "done", "toy", Thought("done", "happy", "I found the toy!")),
    ("wander", "done", "done", None, Thought("done", "happy", "Done!")),
    ("find", "failed", "not_found", "toy", Thought("failed", "sad", "I couldn't find the toy.")),
    ("find", "failed", "lost", "toy", Thought("failed", "sad", "I lost sight of the toy.")),
    (
        "find",
        "failed",
        "outside_area",
        "toy",
        Thought("failed", "sad", "I saw the toy, but it's outside the play area."),
    ),
    ("setxy", "failed", "no_way", None, Thought("failed", "sad", "I can't find a way there.")),
    ("wander", "failed", "blocked", None, Thought("failed", "sad", "I'm blocked.")),
]


@pytest.mark.parametrize(("word", "phase", "result", "thing", "want"), ROWS)
def test_each_row(
    word: str | None, phase: str | None, result: str | None, thing: str | None, want: Thought
) -> None:
    got, _ = Thinking().update(word, phase, result, thing, battery_low=False, learning=None)
    assert got == want


def test_learning_row() -> None:
    got, _ = Thinking().update(None, None, None, None, battery_low=False, learning="cup")
    assert got == Thought("learning", "thinking", "I'm learning what a cup is.")


def test_changed_only_on_new_state_or_thing() -> None:
    t = Thinking()
    _, changed = t.update("find", "drive", None, "toy", battery_low=False, learning=None)
    assert changed
    _, changed = t.update("find", "drive", None, "toy", battery_low=False, learning=None)
    assert not changed
    _, changed = t.update("find", "drive", None, "toy", battery_low=True, learning=None)
    assert not changed  # only the mood: the voice does not say it again
    _, changed = t.update("find", "drive", None, "ball", battery_low=False, learning=None)
    assert changed
    _, changed = t.update("find", "check", None, "ball", battery_low=False, learning=None)
    assert changed


def test_battery_low_is_tired() -> None:
    got, _ = Thinking().update("find", "chase", None, "toy", battery_low=True, learning=None)
    assert got == Thought("chasing", "tired", "I see the toy! Going to it.")


def test_why_three_lines_for_check() -> None:
    lines = why_lines("find", "toy", {"state": "check", "check": (1.2, 0.0)}, (0.0, 0.0), 3.0)
    assert len(lines) == 3
    assert lines[0] == "I want the toy."
    assert lines[1] == "There might be a toy 1.2 m away - I'm going to look closer."


def test_why_counts_maybes() -> None:
    debug: dict[str, object] = {
        "state": "drive",
        "pending": [(1.0, 1.0)],
        "ruled_out": [(2.0, 0.0), (0.5, 0.5)],
    }
    lines = why_lines("find", "toy", debug, (0.0, 0.0), 14.2)
    assert lines[2] == "I've checked 2 of 3 maybes in 14 s."


def test_why_for_wander_and_chase() -> None:
    assert why_lines("wander", None, {"state": "drive", "dest": (1.0, 0.0)}, (0.0, 0.0), 5.0) == [
        "I want to explore the room.",
        "I'm going to a place I haven't seen yet.",
        "I've been exploring for 5 s.",
    ]
    assert (
        why_lines("find", "toy", {"state": "chase"}, (0.0, 0.0), 2.0)[1]
        == "I can see it, so I'm driving to it."
    )
