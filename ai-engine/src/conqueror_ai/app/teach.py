"""Teach the tank a word from the /ai page (AI engine LLD §22.4): check the word, tell the bridge it is
learning (7056 for AI words meanwhile), write it into ai-engine/taught/, make a new detector model with it
in a child process (the event loop goes on; 200 s at most), move the model in and hand it over. Any failure
or a cancel takes the word out again and keeps the old model. A taught word is not measured - that stays
the owner's step."""

from __future__ import annotations

import asyncio
import contextlib
import datetime
import os
import re
import shutil
import signal
import tempfile
from collections.abc import Awaitable, Callable, Sequence
from dataclasses import dataclass
from pathlib import Path

from conqueror_ai.adapters.taught_words import MODEL_NAME, WORDS_TXT, TaughtWords
from conqueror_ai.domain.names import MAX_NAME_BYTES, MAX_NAMES, NEVER, squeeze

ROOT = Path(__file__).resolve().parents[4]  # the repository: ai-engine/src/conqueror_ai/app -> ../../../..
EXPORT_LIMIT_S = 200.0
WORD_RE = re.compile(r"[a-z][a-z -]*")

Export = Callable[[Path, Path], Awaitable[int]]  # (the taught words file, the folder to write) -> exit code


@dataclass(frozen=True)
class Reply:
    ok: bool
    msg: str


def _check(word: str, names: Sequence[str]) -> str | None:
    """Why the word cannot be taught, or None."""
    if word == NEVER:
        return "I never look for people."
    if len(names) > MAX_NAMES:
        return f"At most {MAX_NAMES} names."
    for w in (word, *names):
        if not WORD_RE.fullmatch(w):
            return "Use letters, spaces and - only."
        if len(w.encode()) > MAX_NAME_BYTES:
            return "That word is too long."
    return None


class Teacher:
    def __init__(
        self,
        taught: TaughtWords,
        export: Export,
        *,
        learning: Callable[[str | None], Awaitable[None]],
        installed: Callable[[], Awaitable[None]],
        today: Callable[[], str] = lambda: datetime.date.today().isoformat(),
        limit_s: float = EXPORT_LIMIT_S,
    ) -> None:
        """learning: the word being learnt (None: done) to the bridge and the pages. installed: the new model
        is in taught/ - load it (the detector, the names, the floor words, the things lists)."""
        self.taught, self.export, self.limit_s = taught, export, limit_s
        self.on_learning, self.installed, self.today = learning, installed, today
        self.learning: str | None = None

    async def teach(self, word: str, names: Sequence[str], known: Callable[[str], bool], busy: bool) -> Reply:
        word, names = squeeze(word), [squeeze(n) for n in names if squeeze(n)]
        why = _check(word, names)
        if why is not None:
            return Reply(False, why)
        if known(word):
            return Reply(True, f"I already know {word}.")
        for n in names:
            if known(n):
                return Reply(False, f"{n} is already the name of something I know.")
        if self.learning is not None:
            return Reply(False, "I'm already learning a word.")
        if busy:
            return Reply(False, "I'm busy - try when I've stopped.")
        self.learning = word
        try:
            await self.on_learning(word)
            return await self._learn(word, names)
        finally:
            self.learning = None
            with contextlib.suppress(Exception):  # the link may have gone; the bridge clears it on close
                await asyncio.shield(self.on_learning(None))

    async def _learn(self, word: str, names: list[str]) -> Reply:
        self.taught.add(word, names, self.today())
        done = False
        try:
            with tempfile.TemporaryDirectory(prefix="cq-teach-") as tmp:
                out = Path(tmp)
                try:
                    code = await asyncio.wait_for(self.export(self.taught.vocab_file, out), self.limit_s)
                except TimeoutError:
                    return Reply(False, "Making the new model took too long - I kept the old one.")
                if code != 0 or not (out / MODEL_NAME).is_dir() or not (out / WORDS_TXT).is_file():
                    return Reply(False, "Making the new model failed - I kept the old one.")
                self._move_in(out)
            await self.installed()
            done = True
            return Reply(True, f"I know {word} now.")
        finally:
            if not done:
                self.taught.remove(word)  # words.txt no longer matches: the measured model is used again

    def _move_in(self, out: Path) -> None:
        folder = self.taught.folder
        model = folder / MODEL_NAME
        if model.exists():
            shutil.rmtree(model)
        shutil.move(str(out / MODEL_NAME), model)
        shutil.copyfile(out / WORDS_TXT, folder / WORDS_TXT)


async def run_export(extra: Path, out: Path, *, cmd: Sequence[str] | None = None) -> int:
    """tools/measure/export_world.py --extra EXTRA --out OUT in its own environment, as a child process in
    its own process group (uv starts python under it): a cancel or a timeout kills the whole group."""
    args = (
        list(cmd)
        if cmd is not None
        else [
            "uv",
            "run",
            "--project",
            str(ROOT / "tools" / "measure"),
            "python",
            str(ROOT / "tools" / "measure" / "export_world.py"),
            "--extra",
            str(extra),
            "--out",
            str(out),
        ]
    )
    log_path = extra.parent / "export.log" if cmd is None else Path(os.devnull)  # beside the taught words
    with open(log_path, "w") as log:  # one small file, opened once
        proc = await asyncio.create_subprocess_exec(
            *args, cwd=ROOT, stdout=log, stderr=log, start_new_session=True
        )
        try:
            return await proc.wait()
        except asyncio.CancelledError:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(proc.pid, signal.SIGKILL)
            await proc.wait()
            raise
