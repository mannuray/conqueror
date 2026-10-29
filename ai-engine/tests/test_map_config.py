"""config/map.yaml (AI engine LLD §18.9)."""

from __future__ import annotations

from pathlib import Path

import pytest

from conqueror_ai.adapters.map_config import load_map_config
from conqueror_ai.app.objects import MAP_CONFIG
from conqueror_ai.domain.depth import DepthSettings, Heights, MapConfig, MapSettings, Ranges


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


@pytest.mark.parametrize(
    ("text", "word"),
    [
        ("depth: {every_n: 0}\n", "every_n"),
        ("depth: {every_n: 1.5}\n", "every_n"),
        ("heights: {floor_m: 0.05}\n", "floor_m"),
        ("heights: {low_top_m: 0.30}\n", "low_top_m"),
        ("depth: {trust_deg: 30}\n", "trust_deg"),
    ],
)
def test_settings_that_would_see_less_are_refused(tmp_path: Path, text: str, word: str) -> None:  # I2
    f = tmp_path / "map.yaml"
    f.write_text(text)
    with pytest.raises(ValueError, match=word):
        load_map_config(f)


# ---- the room map's settings (Plan Z4)
def test_map_section_defaults(tmp_path: Path) -> None:
    cfg = load_map_config(tmp_path / "missing.yaml")
    assert cfg.map == MapSettings()
    assert cfg.ranges.sonar_max_m == 4.0


def test_map_section_read(tmp_path: Path) -> None:
    f = tmp_path / "map.yaml"
    f.write_text("map: {hit: 0.9}\n")
    m = load_map_config(f).map
    assert m.hit == 0.9
    assert m == MapSettings(hit=0.9)


@pytest.mark.parametrize(("text", "word"), [("hit: 0.5", "hit"), ("miss: -0.6", "miss")])
def test_map_less_careful_refused(tmp_path: Path, text: str, word: str) -> None:
    f = tmp_path / "map.yaml"
    f.write_text(f"map: {{{text}}}\n")
    with pytest.raises(ValueError, match=word):
        load_map_config(f)


@pytest.mark.parametrize("text", ["size_m: 8.0, cell_m: 0.03", "cell_m: 0", "size_m: -1"])
def test_map_size_must_hold_whole_cells(tmp_path: Path, text: str) -> None:
    f = tmp_path / "map.yaml"
    f.write_text(f"map: {{{text}}}\n")
    with pytest.raises(ValueError):
        load_map_config(f)


def test_repo_map_yaml_loads() -> None:
    assert load_map_config(MAP_CONFIG).map == MapSettings()


# ---- final review I1, I2: "more careful only" the right way round
@pytest.mark.parametrize(
    ("text", "word"),
    [
        ("map: {blocked: 3.9}", "blocked"),
        ("map: {blocked: 0}", "blocked"),
        ("map: {clamp: 0.5}", "clamp"),
        ("map: {clamp: 3.0}", "clamp"),
        ("ranges: {sonar_max_m: 6.0}", "sonar_max_m"),
    ],
)
def test_settings_that_would_make_the_map_less_careful_are_refused(
    tmp_path: Path, text: str, word: str
) -> None:
    f = tmp_path / "map.yaml"
    f.write_text(text + "\n")
    with pytest.raises(ValueError, match=word):
        load_map_config(f)


@pytest.mark.parametrize("text", ["map: {blocked: 0.5}", "map: {clamp: 5.0}", "ranges: {sonar_max_m: 3.0}"])
def test_more_careful_map_settings_are_kept(tmp_path: Path, text: str) -> None:
    f = tmp_path / "map.yaml"
    f.write_text(text + "\n")
    load_map_config(f)


def test_a_positive_miss_is_refused(tmp_path: Path) -> None:  # Z4 review minor (deferred, now fixed)
    f = tmp_path / "map.yaml"
    f.write_text("map: {miss: 0.2}\n")
    with pytest.raises(ValueError, match="miss"):
        load_map_config(f)


@pytest.mark.parametrize(
    "text", ["cost: {keep_out_m: 0.05}\n", "cost: {back_m: 0.3}\n", "cost: {unknown_cost: 0.1}\n"]
)
def test_cost_may_only_be_more_careful(tmp_path: Path, text: str) -> None:  # Plan Z6
    f = tmp_path / "map.yaml"
    f.write_text(text)
    with pytest.raises(ValueError, match="more careful"):
        load_map_config(f)


def test_cost_settings_load(tmp_path: Path) -> None:
    f = tmp_path / "map.yaml"
    f.write_text("cost: {keep_out_m: 0.10, back_m: 0.15}\n")
    c = load_map_config(f).cost
    assert (c.keep_out_m, c.back_m, c.near_cost) == (0.10, 0.15, 0.8)
