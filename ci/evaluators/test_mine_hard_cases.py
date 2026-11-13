"""Unit tests for tools/train_e2e/mine_hard_cases.py."""

from pathlib import Path
import json

from tools.train_e2e.mine_hard_cases import main as mine_main, mine_hard_cases


def test_mine_hard_cases_ranks_safety_and_low_ttc_first():
    records = [
        {"t": 0.1, "jerk": 0.5, "min_ttc_s": 8.0, "confidence": 0.92},
        {"t": 0.2, "jerk": 6.5, "min_ttc_s": 4.0, "confidence": 0.85},
        {"t": 0.3, "jerk": 1.0, "min_ttc_s": 0.7, "confidence": 0.25, "immune_lock": True},
    ]
    mined = mine_hard_cases(records)
    assert len(mined) == 2
    assert mined[0]["t"] == 0.3
    assert set(mined[0]["_hard_case_reasons"]) == {"low_ttc", "low_confidence", "safety_takeover"}
    assert mined[1]["t"] == 0.2
    assert mined[1]["_hard_case_reasons"] == ["high_jerk"]


def test_mine_hard_cases_cli_roundtrip(tmp_path: Path):
    inp = tmp_path / "in.jsonl"
    out = tmp_path / "out.jsonl"
    inp.write_text(
        "\n".join([
            json.dumps({"seq": 1, "jerk": 0.2}),
            json.dumps({"seq": 2, "latency_budget_exceeded": True}),
        ]) + "\n",
        encoding="utf-8",
    )
    rc = mine_main(["--input", str(inp), "--output", str(out)])
    assert rc == 0
    lines = [json.loads(x) for x in out.read_text(encoding="utf-8").splitlines() if x.strip()]
    assert len(lines) == 1
    assert lines[0]["seq"] == 2
    assert lines[0]["_hard_case_reasons"] == ["latency_budget_exceeded"]
