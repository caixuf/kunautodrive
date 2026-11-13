"""Unit tests for ci/evaluators/scenario_regression.py compare_summary."""

from ci.evaluators.scenario_regression import compare_summary


def test_min_ratio_positive_baseline():
    tol = {"min_forward_gap": {"min_ratio": 0.4}}
    assert compare_summary({"min_forward_gap": 10.0}, {"min_forward_gap": 4.0}, tol) == []
    regs = compare_summary({"min_forward_gap": 10.0}, {"min_forward_gap": 3.9}, tol)
    assert len(regs) == 1
    assert "min_forward_gap" in regs[0]


def test_min_ratio_negative_baseline_does_not_tighten():
    # e.g. valet_parking baseline min_road_margin_m = -1.231, ratio = 0.4
    # threshold = -1.231 - 1.231 * 0.6 = -1.9696 (must allow -1.231 itself!)
    tol = {"min_road_margin_m": {"min_ratio": 0.4}}
    assert compare_summary(
        {"min_road_margin_m": -1.231},
        {"min_road_margin_m": -1.231},
        tol,
    ) == []
    assert compare_summary(
        {"min_road_margin_m": -1.231},
        {"min_road_margin_m": -1.90},
        tol,
    ) == []
    regs = compare_summary(
        {"min_road_margin_m": -1.231},
        {"min_road_margin_m": -2.05},
        tol,
    )
    assert len(regs) == 1
    assert "min_road_margin_m" in regs[0]


def test_max_abs_increase():
    tol = {"max_lat_err_m": {"max_abs_increase": 0.25}}
    assert compare_summary({"max_lat_err_m": 0.5}, {"max_lat_err_m": 0.75}, tol) == []
    regs = compare_summary({"max_lat_err_m": 0.5}, {"max_lat_err_m": 0.76}, tol)
    assert len(regs) == 1
    assert "max_lat_err_m" in regs[0]
