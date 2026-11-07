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
from conqueror_ai.domain.names import MAX_NAME_BYTES, MAX_NAMES, squeeze
from conqueror_ai.domain.person_guard import PERSON_WORDS

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
    if any(part in PERSON_WORDS for w in (word, *names) for part in w.split()):
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
        except Exception as e:  # any other failure is an answer too (final review I3); said on the laptop
            print(f"teach {word}: {type(e).__name__}: {e}")
            return Reply(False, "Something went wrong - I kept the old words.")
        finally:
            self.learning = None
            with contextlib.suppress(Exception):  # the link may have gone; the bridge clears it on close
                await asyncio.shield(self.on_learning(None))

    async def _learn(self, word: str, names: list[str]) -> Reply:
        self.taught.add(word, names, self.today())
        done = False
        old = self.taught.folder / "previous"  # the model and words.txt before, put back on a failure
        try:
            with tempfile.TemporaryDirectory(prefix="cq-teach-") as tmp:
                out = Path(tmp)
                try:
                    code = await asyncio.wait_for(self.export(self.taught.vocab_file, out), self.limit_s)
                except TimeoutError:
                    return Reply(False, "Making the new model took too long - I kept the old one.")
                if code != 0 or not (out / MODEL_NAME).is_dir() or not (out / WORDS_TXT).is_file():
                    return Reply(False, "Making the new model failed - I kept the old one.")
                self._move_in(out, old)
            await self.installed()
            done = True
            return Reply(True, f"I know {word} now.")
        finally:
            if not done:
                self.taught.remove(word)
                self._put_back(old)  # the words taught before keep their model (final review I4)
            elif old.exists():
                shutil.rmtree(old)

    def _move_in(self, out: Path, old: Path) -> None:
        """The new model and words.txt into taught/; the ones they replace kept in `old` until it works."""
        folder = self.taught.folder
        if old.exists():
            shutil.rmtree(old)
        old.mkdir(parents=True)
        for name in (MODEL_NAME, WORDS_TXT):
            if (folder / name).exists():
                shutil.move(str(folder / name), old / name)
        shutil.move(str(out / MODEL_NAME), folder / MODEL_NAME)
        shutil.copyfile(out / WORDS_TXT, folder / WORDS_TXT)

    def _put_back(self, old: Path) -> None:
        if not old.exists():
            return
        folder = self.taught.folder
        for name in (MODEL_NAME, WORDS_TXT):
            if (folder / name).is_dir():
                shutil.rmtree(folder / name)
            elif (folder / name).exists():
                (folder / name).unlink()
            if (old / name).exists():
                shutil.move(str(old / name), folder / name)
        shutil.rmtree(old)


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
