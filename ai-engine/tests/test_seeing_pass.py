"""The seeing test's pass (AI engine LLD §28.6): games only after the person test passed with this detector
and this camera calibration."""

from __future__ import annotations

from pathlib import Path

from conqueror_ai.app.seeing_pass import SeeingPass, write_pass

DET = "yoloworld-s-640.mlpackage"
CAL = "2026-10-23"


def test_no_file_no_people(tmp_path: Path) -> None:
    assert not SeeingPass(tmp_path / "person-seeing.yaml", DET, CAL).passed()


def test_a_matching_pass(tmp_path: Path) -> None:
    p = tmp_path / "person-seeing.yaml"
    write_pass(p, DET, CAL, "2026-11-10", {"marks_passed": 14, "marks": 14})
    assert SeeingPass(p, DET, CAL).passed()


def test_another_detector_or_calibration_clears_it(tmp_path: Path) -> None:
    p = tmp_path / "person-seeing.yaml"
    write_pass(p, "yolo26n.pt", CAL, "2026-11-10", {})
    assert not SeeingPass(p, DET, CAL).passed()
    write_pass(p, DET, "2026-11-01", "2026-11-10", {})
    assert not SeeingPass(p, DET, CAL).passed()


def test_a_broken_file_is_no_pass(tmp_path: Path) -> None:
    p = tmp_path / "person-seeing.yaml"
    p.write_text(": not yaml [")
    assert not SeeingPass(p, DET, CAL).passed()
    p.write_text("- a list\n")
    assert not SeeingPass(p, DET, CAL).passed()
