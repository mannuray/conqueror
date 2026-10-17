"""Plan K: the maths of move.py (bridge LLD R7.7)."""

from __future__ import annotations

import pytest

from move import counts_travelled, mean_cpm, new_cpm


def test_counts_travelled_forward_and_reverse() -> None:
    assert counts_travelled((1000, 2000), (8560, 9560), 1) == 7560
    assert counts_travelled((1000, 2000), (-2780, -1780), -1) == 3780


def test_new_cpm_from_tape() -> None:
    assert new_cpm(7560, 100.0) == 7560
    assert new_cpm(7560, 98.5) == 7675  # went less far than counted -> more counts per metre
    assert new_cpm(7642, 101.2) == 7551


def test_new_cpm_rejects_bad_measure() -> None:
    with pytest.raises(ValueError):
        new_cpm(7560, 0.0)


def test_mean_cpm() -> None:
    assert mean_cpm([7675, 7660, 7690]) == 7675
    with pytest.raises(ValueError):
        mean_cpm([])
