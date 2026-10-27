"""config/map.yaml (AI engine LLD §18.9)."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters.map_config import load_map_config
from conqueror_ai.app.objects import MAP_CONFIG
from conqueror_ai.domain.depth import DepthSettings, Heights, MapConfig, Ranges


def test_the_defaults_match_the_file() -> None:
    assert load_map_config(MAP_CONFIG) == MapConfig(Heights(), Ranges(), DepthSettings())


def test_a_lower_clear_height_is_refused(tmp_path: Path) -> None:
    f = tmp_path / "map.yaml"
    f.write_text("heights: {clear_m: 0.15}\n")
    with pytest.raises(ValueError, match="may only be made more careful - clear_m"):
        load_map_config(f)


def test_an_unknown_key_is_refused(tmp_path: Path) -> None:
    f = tmp_path / "map.yaml"
    f.write_text("heights: {foo: 1}\n")
    with pytest.raises(ValueError, match="foo"):
        load_map_config(f)


def test_a_missing_section_keeps_its_defaults(tmp_path: Path) -> None:
    f = tmp_path / "map.yaml"
    f.write_text("depth: {every_n: 2}\n")
    assert load_map_config(f) == MapConfig(Heights(), Ranges(), DepthSettings(every_n=2))
