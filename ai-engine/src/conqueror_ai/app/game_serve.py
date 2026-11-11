"""The skill server's game side (AI engine LLD §28, bridge LLD R34.3): the person words, the game words'
skills, the person stream between `game on` and `game off`, back_ok from the trusted floor and the game's
questions.

The person never resolves as a thing (Names): only the game words and a game run's questions take the person
words, and they answer from PersonWatch - never from the tracker, the memory or the map (Spec 10 S5)."""

from __future__ import annotations

import asyncio
import math
import time
from collections.abc import Awaitable, Callable
from typing import Any

from conqueror_ai.app.world import World
from conqueror_ai.domain.drive.game_skills import EscapeSkill, FaceSkill, FollowSkill
from conqueror_ai.domain.drive.limits import Limits
from conqueror_ai.domain.drive.skill import Skill
from conqueror_ai.domain.drive.trusted import TrustedFloor
from conqueror_ai.domain.names import squeeze
from conqueror_ai.domain.person import PersonWatch

PERSON_WORDS = frozenset({"baby", "person", "kid"})  # the game's person (Tank Logo LLD §27)
GAME_SKILLS = ("face", "escape", "follow")
PERSON_EVERY_S = 0.1  # the stream: at least 10 times a second (>= 5 Hz, R34.2), unseen too
PERSON_POLL_S = 0.02  # ... and each new estimate as soon as it is made (design-notes §237)
SEEN_S = 0.5  # SEE? "baby: a picture with the person this recently
FOLLOW_STOP_CM = 100.0  # FOLLOW's default follow distance
NO_PICTURE_AGE_MS = 60_000  # the stream's age before any picture: never fresh


def is_person(thing: str | None) -> bool:
    return thing is not None and squeeze(thing) in PERSON_WORDS


def game_refusal(skill: str, thing: str | None, cm: float | None, seconds: float | None) -> str | None:
    """Why a game word cannot start (the skill_failed reason), or None: a person word only; escape needs its
    cm, follow its seconds."""
    if not is_person(thing):
        return "unknown_thing"
    if (skill == "escape" and cm is None) or (skill == "follow" and seconds is None):
        return "stale"
    return None


def game_skill(
    skill: str, lim: Limits, back_m: float | None, seconds: float | None, stop_m: float | None
) -> Skill:
    if skill == "face":
        return FaceSkill(lim)
    if skill == "escape":
        return EscapeSkill(lim, back_m or 0.0, TrustedFloor())
    return FollowSkill(lim, seconds or 0.0, stop_m if stop_m is not None else FOLLOW_STOP_CM / 100)


def person_message(watch: PersonWatch, now_us: int) -> dict[str, Any]:
    """The `person` message now: the newest estimate, its age the newest picture's (a frozen camera's grows);
    nobody seen yet: unseen, no distance."""
    e = watch.latest(now_us)
    if e is not None:
        return e.wire(now_us)
    last = watch.last_us
    age = NO_PICTURE_AGE_MS if last is None else max(0, (now_us - last) // 1000)
    return {"t": "person", "seen": False, "d_cm": -1, "bearing": 0, "coming": 0, "sure": 0.0, "age_ms": age}


def back_ok(world: World, cm: int | None, now_us: int) -> bool:
    """May a game's backward move of `cm` run: every cell behind trusted (LLD §28.4)."""
    pose = world.map_run.p.pose()
    if pose is None or cm is None or cm <= 0:
        return False
    at = (pose.x_m, pose.y_m, pose.th_rad)
    return TrustedFloor.ok(world.map_run.room, at, cm / 100, now_us, lim=world.lim)


def person_answer(world: World, q: str, now_us: int) -> bool | int:
    """SEE? / HOWFAR "baby in a game: seen within SEEN_S; the pessimistic distance from the front edge (cm),
    -1 when not seen now."""
    e = world.person.latest(now_us)
    seen = e is not None and e.seen and now_us - e.t_us <= SEEN_S * 1e6
    if q == "howfar":
        return round(e.d_m * 100) if seen and e is not None and e.d_m is not None else -1
    return seen


class PersonStream:
    """The person stream while the bridge's game mode is on; off at every new link (Review Focus 3)."""

    def __init__(
        self,
        world: World,
        send: Callable[[dict[str, Any]], Awaitable[None]],
        bridge_now_s: Callable[[], float | None],
    ) -> None:
        self.world, self.send, self.bridge_now_s = world, send, bridge_now_s
        self.on = False

    async def run(self) -> None:
        last_us: int | None = None  # the newest picture the last message was made from
        last_s = -math.inf
        while True:
            await asyncio.sleep(PERSON_POLL_S)
            now = self.bridge_now_s()
            if not self.on or now is None:
                continue
            fresh = self.world.person.last_us != last_us
            if not fresh and time.monotonic() - last_s < PERSON_EVERY_S - 1e-3:
                continue
            last_us, last_s = self.world.person.last_us, time.monotonic()
            try:
                msg = person_message(self.world.person, round(now * 1e6))
            except Exception as e:  # said, and the stream goes on; the bridge's deadman holds meanwhile (M3)
                print(f"the person stream failed: {type(e).__name__}: {e}")
                continue
            await self.send(msg)
