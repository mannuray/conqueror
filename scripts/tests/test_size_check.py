import importlib.util
import pathlib
import sys

spec = importlib.util.spec_from_file_location("size_check", pathlib.Path(__file__).parents[1] / "size_check.py")
assert spec and spec.loader
sc = importlib.util.module_from_spec(spec)
sys.modules["size_check"] = sc  # dataclasses need the module registered
spec.loader.exec_module(sc)


def test_function_over_limits_is_reported() -> None:
    rows = [sc.Fn(file="a.cpp", name="big", nloc=41, ccn=3), sc.Fn(file="a.cpp", name="ok", nloc=40, ccn=10)]
    assert sc.violations(rows, {"a.cpp": 100}, set()) == ["a.cpp:big  nloc 41 > 40"]


def test_complexity_and_file_size() -> None:
    rows = [sc.Fn(file="b.cpp", name="knot", nloc=10, ccn=11)]
    assert sc.violations(rows, {"b.cpp": 401}, set()) == ["b.cpp  401 lines > 400", "b.cpp:knot  ccn 11 > 10"]


def test_allow_list_silences() -> None:
    rows = [sc.Fn(file="a.cpp", name="big", nloc=90, ccn=30)]
    assert sc.violations(rows, {"a.cpp": 900}, {"a.cpp", "a.cpp:big"}) == []


def test_parse_allow_ignores_comments() -> None:
    assert sc.parse_allow("a.cpp:f  # long table\n\n# note\nb.cpp  # big\n") == {"a.cpp:f", "b.cpp"}
