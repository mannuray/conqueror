"""The seeing test's pass (AI engine LLD §28.6, the owner's lock): `cq-person-test score` writes it on a
full pass - numbers only, no pictures - and the hello's `people` is true only while it names the detector in
use and the camera calibration in use. A new detector or a new calibration clears it: the test runs again."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any, cast

import yaml

PASS_FILE = Path(__file__).resolve().parents[3] / "config" / "person-seeing.yaml"


class SeeingPass:
    def __init__(self, path: Path, detector: str, calibration: str) -> None:
        """detector: the base detector weights' file name; calibration: the camera calibration's date."""
        self.path, self.detector, self.calibration = path, detector, calibration

    def passed(self) -> bool:
        try:
            d: Any = yaml.safe_load(self.path.read_text())
        except (OSError, yaml.YAMLError):
            return False
        if not isinstance(d, dict):
            return False
        doc = cast(dict[str, object], d)
        return doc.get("detector") == self.detector and str(doc.get("calibration")) == self.calibration


def write_pass(path: Path, detector: str, calibration: str, date: str, numbers: Mapping[str, object]) -> None:
    """The pass file: when, which detector and calibration, and the test's numbers."""
    body = {"date": date, "detector": detector, "calibration": calibration, **numbers}
    path.write_text(yaml.safe_dump(body, sort_keys=False))
