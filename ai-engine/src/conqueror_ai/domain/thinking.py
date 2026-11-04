"""What the tank says on the /ai page, in a child's words (AI engine LLD §22.3): the state, the mood and one
sentence from the word running and its skill's phase; and Why? in three short lines from the skill's notes.
Pure: no clock (the time running is handed in)."""

from __future__ import annotations

import math
from collections.abc import Sequence
from dataclasses import dataclass


@dataclass(frozen=True)
class Thought:
    state: str
    mood: str
    say: str


# A failed word says the Logo error's own words (words.yaml 7050-7055); blocked has no error of its own
# (7024 is a move's), so it says it plainly.
FAILED_SAY = {
    "not_found": "I couldn't find the {thing}.",
    "lost": "I lost sight of the {thing}.",
    "outside_area": "I saw the {thing}, but it's outside the play area.",
    "no_way": "I can't find a way there.",
    "blocked": "I'm blocked.",
    "unknown_thing": "I don't know what a {thing} is.",
    "stale": "The AI helper stopped answering.",
    "learning": "I'm learning a new word - try again in a minute.",
}
FIND_SAY = {
    "start": ("looking", "curious", "I'm looking round for the {thing}."),
    "drive": ("searching", "curious", "I'm searching for the {thing}."),
    "check": ("checking", "puzzled", "Is that the {thing}? Let me check."),
    "chase": ("chasing", "excited", "I see the {thing}! Going to it."),
}
UNSTUCK = ("unsticking", "puzzled", "Oops - I'm getting myself unstuck.")


def _thought(word: str | None, phase: str | None, result: str | None, thing: str | None) -> Thought:
    t = thing or "thing"
    if word is None:
        return Thought("ready", "happy", "Ready to play!")
    if result == "done":
        return Thought("done", "happy", f"I found the {t}!" if word == "find" else "Done!")
    if result is not None:
        return Thought("failed", "sad", FAILED_SAY.get(result, "I stopped.").format(thing=t))
    if phase == "recover":
        state, mood, say = UNSTUCK
    elif word == "find":
        state, mood, say = FIND_SAY.get(phase or "start", FIND_SAY["start"])
    elif word == "setxy":
        state, mood, say = "going", "busy", "I'm going there."
    else:
        state, mood, say = "exploring", "curious", "I'm exploring the room."
    return Thought(state, mood, say.format(thing=t))


class Thinking:
    """Keeps the last state and thing, so `changed` is true only when the sentence is new (said once)."""

    def __init__(self) -> None:
        self._last: tuple[str, str | None] | None = None

    def update(
        self,
        word: str | None,
        phase: str | None,
        result: str | None,
        thing: str | None,
        *,
        battery_low: bool,
        learning: str | None,
    ) -> tuple[Thought, bool]:
        if learning is not None:
            th, thing = Thought("learning", "thinking", f"I'm learning what a {learning} is."), learning
        else:
            th = _thought(word, phase, result, thing)
        key = (th.state, thing)
        changed = key != self._last
        self._last = key
        if battery_low:
            th = Thought(th.state, "tired", th.say)
        return th, changed


def _xy(v: object) -> tuple[float, float] | None:
    if isinstance(v, Sequence) and len(v) >= 2:  # pyright: ignore[reportUnknownArgumentType]
        a, b = v[0], v[1]  # pyright: ignore[reportUnknownVariableType]
        if isinstance(a, int | float) and isinstance(b, int | float):
            return float(a), float(b)
    return None


def _count(v: object) -> int:
    return len(v) if isinstance(v, Sequence) else 0  # pyright: ignore[reportUnknownArgumentType]


def why_lines(
    word: str,
    thing: str | None,
    debug: dict[str, object],
    pose_xy: tuple[float, float] | None,
    elapsed_s: float,
) -> list[str]:
    """Purpose, process, performance (§20.7) - only from what the skill's debug() reports."""
    t = thing or "thing"
    state = str(debug.get("state", ""))
    purpose = {"find": f"I want the {t}.", "setxy": "I want to get to the place you gave me."}.get(
        word, "I want to explore the room."
    )
    spot = _xy(debug.get("check"))
    if state == "check" and spot is not None and pose_xy is not None:
        d = math.dist(spot, pose_xy)
        process = f"There might be a {t} {d:.1f} m away - I'm going to look closer."
    elif state == "chase":
        process = "I can see it, so I'm driving to it."
    elif state == "recover":
        process = "I got stuck, so I'm backing out."
    elif state == "start":
        process = "I'm looking round from where I stand."
    elif debug.get("dest") is not None:
        process = (
            "I'm looking where I haven't looked yet."
            if word == "find"
            else "I'm going to a place I haven't seen yet."
        )
    else:
        process = "I'm thinking where to go next."
    secs = f"{elapsed_s:.0f} s"
    if word == "find":
        done = _count(debug.get("ruled_out"))
        performance = f"I've checked {done} of {done + _count(debug.get('pending'))} maybes in {secs}."
    elif word == "setxy":
        performance = f"I've been going for {secs}."
    else:
        performance = f"I've been exploring for {secs}."
    return [purpose, process, performance]
