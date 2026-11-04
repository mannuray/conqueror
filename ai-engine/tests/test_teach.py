"""Teacher: teach the tank a word from the /ai page (AI engine LLD §22.4). The export is replaced by a fake
child process; run_export itself is checked with a stand-in command."""

from __future__ import annotations

import asyncio
import os
import time
from pathlib import Path

import pytest

from conqueror_ai.adapters.taught_words import MODEL_NAME, WORDS_TXT, TaughtWords
from conqueror_ai.app.teach import Reply, Teacher, run_export

KNOWN = {"toy", "car", "ball"}


class Calls:
    def __init__(self) -> None:
        self.log: list[str] = []

    async def learning(self, word: str | None) -> None:
        self.log.append(f"learning {word}")

    async def installed(self) -> None:
        self.log.append("installed")


def ok_export(log: list[str]):  # pyright: ignore[reportUnknownParameterType]
    async def export(extra: Path, out: Path) -> int:
        log.append("export")
        (out / MODEL_NAME).mkdir()
        (out / WORDS_TXT).write_text("toy\ncup\n")
        return 0

    return export


def teacher(tmp: Path, calls: Calls, export, limit_s: float = 5.0) -> Teacher:  # pyright: ignore[reportMissingParameterType, reportUnknownParameterType]
    return Teacher(
        TaughtWords(tmp / "taught"),
        export,  # pyright: ignore[reportUnknownArgumentType]
        learning=calls.learning,
        installed=calls.installed,
        today=lambda: "2026-11-04",
        limit_s=limit_s,
    )


async def teach(t: Teacher, word: str, names: list[str], busy: bool = False) -> Reply:
    return await t.teach(word, names, lambda n: n in KNOWN, busy)


@pytest.mark.parametrize(
    ("word", "names", "msg"),
    [
        ("cup!", [], "Use letters, spaces and - only."),
        ("a" * 25, [], "That word is too long."),
        ("cup", [f"n{i}" for i in range(9)], "At most 8 names."),
        ("person", [], "I never look for people."),
        ("cup", ["mug2"], "Use letters, spaces and - only."),
        ("", [], "Use letters, spaces and - only."),
        ("kid", [], "I never look for people."),
        ("cup", ["baby"], "I never look for people."),
        ("Little Boy", [], "I never look for people."),
    ],
)
async def test_bad_word_refused(tmp_path: Path, word: str, names: list[str], msg: str) -> None:
    calls = Calls()
    r = await teach(teacher(tmp_path, calls, ok_export(calls.log)), word, names)
    assert r == Reply(False, msg)
    assert calls.log == []


async def test_known_word_ok_no_export(tmp_path: Path) -> None:
    calls = Calls()
    r = await teach(teacher(tmp_path, calls, ok_export(calls.log)), "Car", [])
    assert r == Reply(True, "I already know car.")
    assert calls.log == []


async def test_refused_while_busy(tmp_path: Path) -> None:
    calls = Calls()
    r = await teach(teacher(tmp_path, calls, ok_export(calls.log)), "cup", [], busy=True)
    assert r == Reply(False, "I'm busy - try when I've stopped.")


async def test_success_swaps_and_learning_on_off(tmp_path: Path) -> None:
    calls = Calls()
    t = teacher(tmp_path, calls, ok_export(calls.log))
    r = await teach(t, "Cup", ["mug"])
    assert r == Reply(True, "I know cup now.")
    assert calls.log == ["learning cup", "export", "installed", "learning None"]
    taught = tmp_path / "taught"
    assert (taught / MODEL_NAME).is_dir() and (taught / WORDS_TXT).read_text() == "toy\ncup\n"
    assert [w.word for w in TaughtWords(taught).words()] == ["cup"]
    assert t.learning is None


async def test_one_teach_at_a_time(tmp_path: Path) -> None:
    calls = Calls()
    gate = asyncio.Event()

    async def slow(extra: Path, out: Path) -> int:
        await gate.wait()
        return 1

    t = teacher(tmp_path, calls, slow)
    first = asyncio.create_task(teach(t, "cup", []))
    await asyncio.sleep(0.01)
    assert t.learning == "cup"
    assert await teach(t, "sock", []) == Reply(False, "I'm already learning a word.")
    gate.set()
    await first


async def test_failed_export_removes_entry_keeps_model(tmp_path: Path) -> None:
    calls = Calls()
    old = tmp_path / "taught" / MODEL_NAME
    old.mkdir(parents=True)
    (old / "marker").write_text("old")

    async def failing(extra: Path, out: Path) -> int:
        return 1

    r = await teach(teacher(tmp_path, calls, failing), "cup", [])
    assert r == Reply(False, "Making the new model failed - I kept the old one.")
    assert TaughtWords(tmp_path / "taught").words() == []
    assert (old / "marker").read_text() == "old"
    assert calls.log == ["learning cup", "learning None"]


async def test_timeout_kept_old_model(tmp_path: Path) -> None:
    calls = Calls()

    async def hangs(extra: Path, out: Path) -> int:
        await asyncio.sleep(10)
        return 0

    r = await teach(teacher(tmp_path, calls, hangs, limit_s=0.1), "cup", [])
    assert r == Reply(False, "Making the new model took too long - I kept the old one.")
    assert TaughtWords(tmp_path / "taught").words() == []
    assert calls.log[-1] == "learning None"


async def test_cancel_removes_entry(tmp_path: Path) -> None:
    calls = Calls()

    async def hangs(extra: Path, out: Path) -> int:
        await asyncio.sleep(10)
        return 0

    task = asyncio.create_task(teach(teacher(tmp_path, calls, hangs), "cup", []))
    await asyncio.sleep(0.05)
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert TaughtWords(tmp_path / "taught").words() == []
    assert calls.log[-1] == "learning None"


async def test_run_export_kills_group_on_cancel(tmp_path: Path) -> None:
    pid_file = tmp_path / "pid"
    cmd = ["sh", "-c", f"sleep 30 & echo $! > {pid_file}; wait"]  # a child of the child, as uv -> python
    task = asyncio.create_task(run_export(tmp_path / "x.yaml", tmp_path / "out", cmd=cmd))
    for _ in range(100):
        if pid_file.exists() and pid_file.read_text().strip():
            break
        await asyncio.sleep(0.02)
    grandchild = int(pid_file.read_text())
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    t0 = time.monotonic()
    while time.monotonic() - t0 < 2.0:
        try:
            os.kill(grandchild, 0)
        except ProcessLookupError:
            break
        await asyncio.sleep(0.05)
    with pytest.raises(ProcessLookupError):
        os.kill(grandchild, 0)


async def test_any_failure_still_answers(tmp_path: Path) -> None:
    """Final review I3: uv missing (or any error) is an answer, never a page left on "Learning..."."""
    calls = Calls()

    async def no_uv(extra: Path, out: Path) -> int:
        raise FileNotFoundError("uv")

    r = await teach(teacher(tmp_path, calls, no_uv), "cup", [])
    assert r == Reply(False, "Something went wrong - I kept the old words.")
    assert TaughtWords(tmp_path / "taught").words() == []
    assert calls.log[-1] == "learning None"


async def test_a_failed_install_keeps_the_earlier_taught_model(tmp_path: Path) -> None:
    """Final review I4: the words taught before survive a failure after the new model was moved in."""
    taught = tmp_path / "taught"
    TaughtWords(taught).add("sock", [], "2026-11-03")
    old = taught / MODEL_NAME
    old.mkdir(parents=True)
    (old / "marker").write_text("sock model")
    (taught / WORDS_TXT).write_text("toy\nsock\n")
    calls = Calls()

    async def broken_install() -> None:
        raise RuntimeError("the new model did not load")

    t = Teacher(
        TaughtWords(taught),
        ok_export(calls.log),
        learning=calls.learning,
        installed=broken_install,
        today=lambda: "2026-11-04",
    )
    r = await teach(t, "cup", [])
    assert not r.ok
    assert [w.word for w in TaughtWords(taught).words()] == ["sock"]
    assert (old / "marker").read_text() == "sock model"
    assert (taught / WORDS_TXT).read_text() == "toy\nsock\n"


async def test_run_export_returns_the_exit_code(tmp_path: Path) -> None:
    assert await run_export(tmp_path / "x.yaml", tmp_path / "out", cmd=["sh", "-c", "exit 3"]) == 3
