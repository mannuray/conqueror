"""Questions: SEE? and HOWFAR from the latest World (AI engine LLD §21.3)."""

from __future__ import annotations

import math

import pytest

from conqueror_ai.app.questions import SEEN_S, Questions
from conqueror_ai.app.world import World
from conqueror_ai.domain.types import ObjectEstimate, Pose
from tests.test_world import world


def thing(cls: str, x: float, y: float, last_s: float, state: str = "seen") -> ObjectEstimate:
    return ObjectEstimate(1, cls, x, y, 0.0, 0.0, 0.05, 0, round(last_s * 1e6), state)


def with_things(w: World, mp: pytest.MonkeyPatch, things: list[ObjectEstimate], pose: Pose | None) -> World:
    mp.setattr(w.map_run.p, "objects", lambda: things)
    mp.setattr(w.map_run.p, "pose", lambda: pose)
    return w


FACING_X = Pose(0, 0.0, 0.0, 0.0, 0.0, 0.0)


def test_seen_within_half_second(monkeypatch: pytest.MonkeyPatch) -> None:
    w = with_things(world(), monkeypatch, [thing("toy", 1.0, 0.0, 10.0)], FACING_X)
    q = Questions()
    assert q.see(w, "toy", 10.0 + SEEN_S - 0.01)
    assert not q.see(w, "toy", 10.0 + SEEN_S + 0.01)  # listed, but not seen lately
    assert not q.see(w, "box", 10.0)  # not listed
    w = with_things(world(), monkeypatch, [thing("toy", 1.0, 0.0, 10.0, "remembered")], FACING_X)
    assert not q.see(w, "toy", 10.0)  # remembered is not seen now


def test_see_ball_means_any_ball(monkeypatch: pytest.MonkeyPatch) -> None:
    w = with_things(world(), monkeypatch, [thing("ball:red", 1.0, 0.0, 5.0)], FACING_X)
    assert Questions().see(w, "ball", 5.1)
    assert not Questions().see(w, "chair", 5.1)


def test_howfar_nearest_or_minus_one(monkeypatch: pytest.MonkeyPatch) -> None:
    a, b = thing("toy", 1.0, 0.0, 1.0), thing("toy", 0.0, -0.5, 1.0)
    w = with_things(world(), monkeypatch, [a, b, thing("box", 0.2, 0.0, 1.0)], FACING_X)
    lens = w.lim.cam_x_m
    q = Questions()
    assert q.howfar(w, "toy", 1.0) == round(math.hypot(lens, 0.5) * 100)  # b is nearer; from the lens
    only_a = with_things(world(), monkeypatch, [a], FACING_X)
    assert q.howfar(only_a, "toy", 1.0) == round((1.0 - lens) * 100)  # not the tank's centre
    assert q.howfar(w, "chair", 1.0) == -1
    # the lens leads the centre along the heading
    turned = Pose(0, 0.0, 0.0, math.pi / 2, 0.0, 0.0)
    w2 = with_things(world(), monkeypatch, [thing("toy", 0.0, 1.0, 1.0)], turned)
    assert q.howfar(w2, "toy", 1.0) == round((1.0 - lens) * 100)
    w3 = with_things(world(), monkeypatch, [a], None)  # no pose yet
    assert q.howfar(w3, "toy", 1.0) == -1
