"""Plan N3: the text logo.py prints and its run loop (Tank Logo LLD §20.4)."""

from __future__ import annotations

from typing import Any

from logo import check_lines, event_line, run_program


def test_check_lines_ok_with_words_and_path() -> None:
    reply: dict[str, Any] = {
        "ok": True,
        "prog": 17,
        "diags": [],
        "words": [{"name": "SQUARE", "inputs": 1, "gives": False}],
        "path": [[0, 0, 0, 0, 0], [0, 500, 0, 1, 0], [0, 500, 90, 1, 1]],
        "cut_short": False,
        "sim_error": None,
    }
    lines = check_lines(reply)
    assert lines[0] == "your word SQUARE (1 inputs)"
    assert lines[1] == "path: 3 points, ends at x 0.0 cm, y 50.0 cm, heading 90° (1 uncertain)"
    assert lines[-1] == "OK - can run (program 17)"


def test_check_lines_errors() -> None:
    reply: dict[str, Any] = {
        "ok": False,
        "prog": 3,
        "diags": [
            {"code": 7001, "line": 2, "col": 1, "msg": "I don't know how to FOO.", "warning": False}
        ],
        "path": [],
    }
    assert check_lines(reply) == [
        "line 2, col 1: error 7001: I don't know how to FOO.",
        "NOT runnable (program 3)",
    ]


def test_event_lines() -> None:
    assert event_line({"kind": "logo_line", "line": 3, "round": 2}) == "line 3 (round 2)"
    finished = {"kind": "logo_ended", "reason": "finished", "line": 4, "msg": ""}
    assert event_line(finished) == "finished"
    blocked = {"kind": "logo_ended", "reason": 7024, "line": 2, "msg": "x"}
    assert event_line(blocked) == "ended 7024 on line 2: x"
    assert event_line({"kind": "primitive_done"}) is None


class FakeWs:
    def __init__(self, replies: list[dict[str, Any]]) -> None:
        self.sent: list[dict[str, Any]] = []
        self.replies = replies

    def send(self, obj: dict[str, Any]) -> None:
        self.sent.append(obj)

    def recv(self, timeout: float) -> dict[str, Any] | None:
        return self.replies.pop(0) if self.replies else None

    def close(self) -> None:
        pass


def test_run_until_ended_and_step() -> None:
    ws = FakeWs(
        [
            {"t": "ack", "id": 1},
            {"t": "ev", "kind": "logo_line", "line": 1, "round": 0},
            {"t": "ev", "kind": "logo_pose", "x_cm": 0, "y_cm": 0, "heading": 0},
            {"t": "ev", "kind": "logo_ended", "reason": "finished", "line": 1, "msg": ""},
        ]
    )
    shown: list[str] = []
    ended = run_program(ws, 17, True, shown.append, lambda: True, clock=lambda: 0.0)
    assert ended is not None and ended["reason"] == "finished"
    assert ws.sent[0] == {"t": "logo_run", "prog": 17, "step": True, "id": 1}
    assert ws.sent[1] == {"t": "logo_step", "id": 2}
    assert shown == ["line 1", "finished"]


def test_run_refused() -> None:
    ws = FakeWs([{"t": "nack", "id": 1, "reason": "CHECK_AGAIN", "message": "check it again"}])
    shown: list[str] = []
    assert run_program(ws, 9, False, shown.append) is None
    assert shown == ["refused: CHECK_AGAIN - check it again"]


def test_pings_once_a_second_while_events_keep_coming() -> None:
    events: list[dict[str, Any]] = [{"t": "ev", "kind": "logo_pose"} for _ in range(30)]
    events.append({"t": "ev", "kind": "logo_ended", "reason": "finished", "line": 1, "msg": ""})
    ws = FakeWs(events)
    t = [0.0]

    def clock() -> float:
        t[0] += 0.1  # an event every 0.1 s (logo_pose at 10 Hz)
        return t[0]

    run_program(ws, 5, False, lambda _: None, clock=clock)
    pings = [m for m in ws.sent if m.get("t") == "ping"]
    assert len(pings) >= 2  # the bridge reaps a client silent for 3 s


def test_step_mode_keeps_pinging_while_waiting_for_enter() -> None:
    # final review I1: the stepper is polled, never blocks: pings go on while the child reads
    events: list[dict[str, Any] | None] = [{"t": "ev", "kind": "logo_line", "line": 1, "round": 0}]
    events += [None] * 12  # 12 s with nothing from the bridge
    events.append({"t": "ev", "kind": "logo_ended", "reason": "finished", "line": 1, "msg": ""})
    ws = FakeWs(events)  # pyright: ignore[reportArgumentType]
    t = [0.0]

    def clock() -> float:
        t[0] += 0.5
        return t[0]

    presses = [False] * 8 + [True]  # Enter after about 4 s

    run_program(
        ws, 5, True, lambda _: None, lambda: presses.pop(0) if presses else True, clock=clock
    )
    sent = [m.get("t") for m in ws.sent]
    first_step = sent.index("logo_step")
    assert sent[:first_step].count("ping") >= 3  # pinged while waiting for Enter
