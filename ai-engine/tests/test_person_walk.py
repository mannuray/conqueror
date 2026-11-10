"""The simple seeing test (design-notes §234): one crawl straight ahead, the sonar as the ruler."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters import calibration_store
from conqueror_ai.adapters.session import SessionRecorder
from conqueror_ai.app.objects import CALIBRATION, VGA
from conqueror_ai.app.person_test import Unlock
from conqueror_ai.app.person_walk import walk_samples, walk_score_folder
from conqueror_ai.domain.person_test import BANDS, PersonRange, score_walk
from conqueror_ai.domain.types import Frame
from tests.test_person_test_app import VGA_JPEG, PersonAt, td


def at(d: float) -> PersonRange:
    return PersonRange(d, 0.0, "")


def crawl(seen: bool = True, off: float = 0.0) -> list[tuple[float | None, PersonRange | None]]:
    """Ten pictures in every band, the camera reading the sonar's distance (+ off)."""
    out: list[tuple[float | None, PersonRange | None]] = []
    for b in BANDS:
        for k in range(10):
            sonar = b.lo + (b.hi - b.lo) * (k + 0.5) / 10
            out.append((sonar, at(sonar + off) if seen else None))
    return out


def test_a_good_crawl_passes_every_band() -> None:
    scores = score_walk(crawl())
    assert [s.band.name for s in scores] == ["0.3", "0.5", "1.0", "1.5", "2.0"]
    assert all(s.passed for s in scores)


def test_a_camera_reading_too_far_fails_from_half_a_metre() -> None:
    scores = {s.band.name: s for s in score_walk(crawl(off=0.25))}
    assert scores["1.0"].passed is False and "distance" in scores["1.0"].why
    assert scores["0.3"].passed  # the sonar's case: the camera is not scored for distance there


def test_unseen_fails_and_a_band_never_reached_fails() -> None:
    scores = {s.band.name: s for s in score_walk(crawl(seen=False))}
    assert not scores["1.0"].passed and "seen" in scores["1.0"].why
    near_only = [x for x in crawl() if x[0] is not None and x[0] < 0.6]
    far = {s.band.name: s for s in score_walk(near_only)}
    assert not far["2.0"].passed and "pictures" in far["2.0"].why


def test_a_box_cut_by_the_bottom_counts_as_seen_close() -> None:
    cut = [(s, PersonRange(None, None, "cut")) for s, _ in crawl() if s is not None and s < 0.4]
    (b,) = [s for s in score_walk(cut) if s.band.name == "0.3"]
    assert b.passed and b.seen == len(cut)


def test_no_sonar_reading_is_left_out() -> None:
    scores = score_walk([(None, at(1.0))] * 20 + crawl())
    assert all(s.passed for s in scores)


def walk(folder: Path, dist_mm: int, n: int = 40) -> None:
    rec = SessionRecorder(folder, {"what": "test"})
    for i in range(n):
        t = 100.0 + i * 0.1
        rec.telemetry(t - 0.01, td(i, 105.0, dist_mm))
        rec.frame(Frame(i, i * 100_000, t, VGA_JPEG))
    rec.close()


def test_samples_pair_each_picture_with_the_sonar(tmp_path: Path) -> None:
    cal = calibration_store.load(CALIBRATION, VGA)
    walk(tmp_path / "w", 1020)
    got = walk_samples(tmp_path / "w", cal, PersonAt(1.0), None, None)
    assert len(got) >= 30
    sonar, pick = got[-1]
    assert sonar == pytest.approx(1.02) and pick is not None and pick.dist_m == pytest.approx(1.0, abs=0.05)


def test_walk_score_writes_the_pass_only_when_every_band_passes(tmp_path: Path) -> None:
    cal = calibration_store.load(CALIBRATION, VGA)
    walk(tmp_path / "w", 1020)  # only the 1 m band: not a pass
    u = Unlock("fake", tmp_path / "pass.yaml", "yoloworld-s-640.mlpackage", "2026-10-23", ())
    u.path.write_text("old: pass\n")
    text = walk_score_folder(tmp_path / "w", cal, PersonAt(1.0), None, None, u, "fake")
    assert "1.0: pass" in text and "games locked" in text and not u.path.exists()
