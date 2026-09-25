#!/usr/bin/env python3
"""tests/test_lane_match_schema_check.py — lane_match cJSON schema gate 单测.

对应 REQ_L3_DIR2_HDMAP.md §6.1 (v1.0) 的 M2 字段集：
  llt_id / llt_s / llt_offset / llt_heading_err_rad / valid

覆盖 5 个用例（与 v1.0 spec §7.1 A1~A5 一一对应）：
  1. valid=1 完整 payload → PASS
  2. valid=0 退化 payload → PASS
  3. 缺 1 个字段 → FAIL 并指出哪个字段
  4. valid=1 但 llt_id=0 → FAIL（匹配失效与"声称匹配"矛盾）
  5. valid 写成 bool True → FAIL（拒绝 bool 假扮 int）
  6. M3 风格的额外字段（curvature/lane_width/...）→ PASS（前向兼容）
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "ci" / "gates"))

from lane_match_schema_check import (  # noqa: E402
    REQUIRED_FIELDS,
    _check_payload,
    main as gate_main,
)


def _valid_payload() -> dict:
    return {
        "id_mismatch_frames": 0,
        "frames": 100,
        "llt_id": 42,
        "llt_s": 12.5,
        "llt_offset": -0.1,
        "llt_heading_err_rad": 0.02,
        "valid": 1,
    }


def _degraded_payload() -> dict:
    return {
        "id_mismatch_frames": 5,
        "frames": 50,
        "llt_id": 0,
        "llt_s": 0.0,
        "llt_offset": 0.0,
        "llt_heading_err_rad": 0.0,
        "valid": 0,
    }


class TestSchemaGate(unittest.TestCase):

    def test_01_valid_payload_passes(self) -> None:
        """A1: 完整 5 字段 + valid=1, llt_id>0 → PASS"""
        errs = _check_payload(_valid_payload(), "test_01")
        self.assertEqual(errs, [], f"unexpected errors: {errs}")

    def test_02_degraded_payload_passes(self) -> None:
        """A2: valid=0 时所有 llt_* 允许 0 → PASS"""
        errs = _check_payload(_degraded_payload(), "test_02")
        self.assertEqual(errs, [], f"unexpected errors: {errs}")

    def test_03_missing_one_field_fails(self) -> None:
        """A3: 缺 1 个字段（valid） → FAIL 并点名"""
        payload = _valid_payload()
        del payload["valid"]
        errs = _check_payload(payload, "test_03")
        self.assertEqual(len(errs), 1, f"expected 1 error, got {len(errs)}: {errs}")
        self.assertIn("missing required field 'valid'", errs[0])

    def test_04_valid_one_with_zero_llt_id_fails(self) -> None:
        """A4: valid=1 但 llt_id=0 → FAIL（语义矛盾）"""
        payload = _valid_payload()
        payload["valid"] = 1
        payload["llt_id"] = 0
        errs = _check_payload(payload, "test_04")
        self.assertEqual(len(errs), 1, f"expected 1 error, got {len(errs)}: {errs}")
        self.assertIn("valid=1 requires llt_id > 0", errs[0])

    def test_05_valid_as_bool_true_fails(self) -> None:
        """A5: valid 写成 True（Python bool 是 int 子类） → FAIL"""
        payload = _valid_payload()
        payload["valid"] = True
        errs = _check_payload(payload, "test_05")
        self.assertEqual(len(errs), 1, f"expected 1 error, got {len(errs)}: {errs}")
        self.assertIn("is bool", errs[0])

    def test_06_m3_extra_fields_pass(self) -> None:
        """A6: M3 风格的 5 个额外字段（前向兼容） → PASS"""
        payload = _valid_payload()
        payload.update({
            "curvature": 0.001,
            "lane_width": 3.5,
            "left_lanelet_id": 41,
            "right_lanelet_id": 43,
            "flags": 0,
        })
        errs = _check_payload(payload, "test_06")
        self.assertEqual(errs, [], f"unexpected errors: {errs}")

    def test_07_each_required_field_in_order(self) -> None:
        """A7: REQUIRED_FIELDS 必须是 5 个且名称正确（防止后续误删）"""
        names = tuple(k for k, _ in REQUIRED_FIELDS)
        self.assertEqual(
            names,
            ("llt_id", "llt_s", "llt_offset", "llt_heading_err_rad", "valid"),
        )

    def test_08_cli_runs_against_fixture(self) -> None:
        """A8: CLI 入口（main）对 fixture 文件能 PASS / FAIL 正确返回码"""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            good = tmp / "good.json"
            bad = tmp / "bad.json"
            good.write_text(json.dumps(_valid_payload()), encoding="utf-8")
            bad.write_text(json.dumps({"llt_id": 0}), encoding="utf-8")

            # 仅 good → exit 0
            rc = gate_main([str(good)])
            self.assertEqual(rc, 0, "good fixture should exit 0")

            # 仅 bad → exit 1
            rc = gate_main([str(bad)])
            self.assertEqual(rc, 1, "bad fixture should exit 1")

            # 好+坏混合 → exit 1
            rc = gate_main([str(good), str(bad)])
            self.assertEqual(rc, 1, "mixed should exit 1")

            # --self-test → exit 0
            rc = gate_main(["--self-test"])
            self.assertEqual(rc, 0, "self-test should exit 0")


if __name__ == "__main__":
    unittest.main()
