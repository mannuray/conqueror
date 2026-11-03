"""One name each for shared values (AI engine LLD §19.2, review F5)."""

from __future__ import annotations


def test_one_name_each() -> None:
    from conqueror_ai.domain import reach
    from conqueror_ai.domain.drive import candidates, chase, goals, roam
    from conqueror_ai.domain.drive.limits import Limits

    assert (reach.SAME_SPOT_M, reach.SURE_REACH_M) == (0.5, 1.0)
    assert (Limits().slow_cms, Limits().drive_tilt_deg) == (18.0, -15.0)
    gone = (
        (chase, "SLOW_CMS"),
        (roam, "SLOW_CMS"),
        (chase, "DRIVE_TILT"),
        (candidates, "MERGE_M"),
        (candidates, "CLOSE_M"),
        (goals, "LOOK_M"),
    )
    for mod, name in gone:
        assert not hasattr(mod, name), (mod.__name__, name)
