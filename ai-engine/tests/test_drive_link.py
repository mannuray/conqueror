"""The drive link's speed conversion and the drive config (AI engine LLD §17.7)."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters.drive_config import load_drive
from conqueror_ai.adapters.drive_link import drive_msg, to_percent
from conqueror_ai.domain.drive.curves import Command

ENGINE = Path(__file__).resolve().parents[1]


def test_speed_to_percent_follows_the_measured_table() -> None:
    assert to_percent(0.0) == 0
    assert to_percent(17.7) == 15  # the lowest smooth speed
    assert to_percent(22.0) == 40
    assert to_percent(25.0) == 56  # between 22 (40 %) and 33.5 (100 %): 55.7
    assert to_percent(40.0) == 100


def test_a_drive_message_is_drive_rate_with_the_turn_rate_plus_left() -> None:
    assert drive_msg(Command(25.0, -12.34)) == {"t": "drive_rate", "v": 56, "rate": -12.3}


def test_the_drive_config_file_loads_and_checks(tmp_path: Path) -> None:
    lim, table = load_drive(ENGINE / "config" / "drive.yaml")
    assert lim.v_cap_cms == 25.0 and table[1] == (17.7, 15.0)
    bad = tmp_path / "drive.yaml"
    bad.write_text("limits: {top_speed: 3}\n")
    with pytest.raises(ValueError, match="unknown limits top_speed"):
        load_drive(bad)
    bad.write_text("speed_table: [[0, 0], [20, 40], [10, 50]]\n")
    with pytest.raises(ValueError, match="must rise"):
        load_drive(bad)
    assert load_drive(tmp_path / "missing.yaml")[0] == load_drive(ENGINE / "missing.yaml")[0]
