#!/usr/bin/env python3
"""tests/test_lane_match_schema_check.py — lane_match cJSON schema gate 单测.

对应 REQ_L3_DIR2_HDMAP.md §6.1 (v1.0) 的字段集：
  M2 (5): llt_id / llt_s / llt_offset / llt_heading_err_rad / valid
  M3 (5): curvature / lane_width / left_lanelet_id / right_lanelet_id / flags

Test classes:
  TestSchemaGate   — M2 字段集回归 + REQUIRED_FIELDS schema 锁定 (8 cases)
  TestM3Fields     — M3 字段集扩展 + 严格模式 (≥6 cases)
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
    M3_FIELDS,
    REQUIRED_FIELDS,
    _check_payload,
    main as gate_main,
)


# --- shared fixture helpers ----------------------------------------------

def _valid_payload() -> dict:
    """M2: complete 5-field payload, valid=1, llt_id>0."""
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
    """M2: valid=0 退化 payload, all llt_* == 0."""
    return {
        "id_mismatch_frames": 5,
        "frames": 50,
        "llt_id": 0,
        "llt_s": 0.0,
        "llt_offset": 0.0,
        "llt_heading_err_rad": 0.0,
        "valid": 0,
    }


def _m3_full_payload() -> dict:
    """M3: full 10-field payload, M2 + M3, all valid values."""
    p = _valid_payload()
    p.update({
        "curvature": 0.0025,         # 1/m, slight left curve
        "lane_width": 3.5,           # m
        "left_lanelet_id": 41,       # uint64, 0 = no left neighbor
        "right_lanelet_id": 43,      # uint64, 0 = no right neighbor
        "flags": 0,                  # uint32 ABI reserved, set 0 for now
    })
    return p


# --- TestSchemaGate: M2 baseline -----------------------------------------

class TestSchemaGate(unittest.TestCase):
    """M2 字段集回归 — 与 v1.0 spec §7.1 A1~A5 + 额外契约守门."""

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

    def test_07_required_fields_locked(self) -> None:
        """A7: REQUIRED_FIELDS 必须是 10 个且按 spec §6.1 v1.0 顺序.
        防止后续误删 / 重排破坏契约.
        """
        names = tuple(k for k, _ in REQUIRED_FIELDS)
        self.assertEqual(
            names,
            (
                # M2 — 顺序与原 M2 严格保持
                "llt_id", "llt_s", "llt_offset", "llt_heading_err_rad", "valid",
                # M3 — spec §6.2 追加顺序
                "curvature", "lane_width",
                "left_lanelet_id", "right_lanelet_id", "flags",
            ),
        )
        # 类型 kind 也锁定（防止 silent 把 "int" 改成 "number"）
        kinds = tuple(k for _, k in REQUIRED_FIELDS)
        self.assertEqual(
            kinds,
            ("int", "number", "number", "number", "int",
             "number", "number", "int", "int", "int"),
        )
        # M3_FIELDS 必须是 5 个且与 REQUIRED_FIELDS 后 5 项完全一致
        self.assertEqual(
            M3_FIELDS,
            frozenset({"curvature", "lane_width",
                       "left_lanelet_id", "right_lanelet_id", "flags"}),
        )
        self.assertEqual(
            M3_FIELDS,
            frozenset(name for name, _ in REQUIRED_FIELDS[5:]),
        )

    def test_08_cli_runs_against_fixture(self) -> None:
        """A8: CLI 入口（main）对 fixture 文件能 PASS / FAIL 正确返回码."""
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


# --- TestM3Fields: M3 schema expansion -----------------------------------

class TestM3Fields(unittest.TestCase):
    """M3 schema 扩展（spec §6.1 v1.0 + §6.2 追加字段）.

    覆盖 spec §6.2 的 5 个新字段 + --strict-m3 严格模式 + 类型守门.
    所有用例通过 tempfile 构造 JSON fixture，不依赖真实 map.
    """

    def test_09_m3_full_10_fields_passes(self) -> None:
        """M3 全 10 字段齐 + 类型正确 → PASS."""
        errs = _check_payload(_m3_full_payload(), "test_09")
        self.assertEqual(errs, [], f"unexpected errors: {errs}")

    def test_10_m2_only_passes_in_default_mode(self) -> None:
        """M3 默认模式: M2-only payload 仍 PASS（向后兼容 M2 producer）."""
        payload = _valid_payload()  # 仅 M2 5 字段
        errs = _check_payload(payload, "test_10")
        self.assertEqual(errs, [], f"unexpected errors: {errs}")
        # strict 模式下应该 FAIL（M3 字段缺失）
        errs_strict = _check_payload(payload, "test_10", strict_m3=True)
        self.assertTrue(
            any("'curvature'" in e and "missing" in e for e in errs_strict),
            f"strict mode should report missing curvature: {errs_strict}",
        )

    def test_11_m3_curvature_as_string_fails(self) -> None:
        """M3: curvature="0.001"（str）→ FAIL（必须是 number）."""
        payload = _m3_full_payload()
        payload["curvature"] = "0.001"  # WRONG: str
        errs = _check_payload(payload, "test_11")
        self.assertEqual(len(errs), 1, f"expected 1 error, got {len(errs)}: {errs}")
        self.assertIn("'curvature'", errs[0])
        self.assertIn("is str", errs[0])
        self.assertIn("expected number", errs[0])

    def test_12_m3_left_lanelet_id_as_bool_fails(self) -> None:
        """M3: left_lanelet_id=True（bool）→ FAIL（必须 strict int）."""
        payload = _m3_full_payload()
        payload["left_lanelet_id"] = True  # WRONG: bool 是 int 子类
        errs = _check_payload(payload, "test_12")
        self.assertEqual(len(errs), 1, f"expected 1 error, got {len(errs)}: {errs}")
        self.assertIn("'left_lanelet_id'", errs[0])
        self.assertIn("is bool", errs[0])
        self.assertIn("expected int", errs[0])

    def test_13_m3_right_lanelet_id_as_float_fails(self) -> None:
        """M3: right_lanelet_id=43.0（float）→ FAIL（uint64 必须是 strict int）.

        cJSON 区分 int-valued 与 float-valued，43.0 在 producer 端几乎
        总是 bug（混淆了 .0 后缀）；门禁要 surface 这个.
        """
        payload = _m3_full_payload()
        payload["right_lanelet_id"] = 43.0  # WRONG: float
        errs = _check_payload(payload, "test_13")
        self.assertEqual(len(errs), 1, f"expected 1 error, got {len(errs)}: {errs}")
        self.assertIn("'right_lanelet_id'", errs[0])
        self.assertIn("is float", errs[0])
        self.assertIn("expected int", errs[0])

    def test_14_strict_m3_missing_field_fails(self) -> None:
        """M3 strict mode: M3 字段缺失 → FAIL."""
        payload = _valid_payload()
        payload.update({
            "curvature": 0.001,
            "lane_width": 3.5,
            # left_lanelet_id intentionally missing
            "right_lanelet_id": 43,
            "flags": 0,
        })
        # 默认模式 PASS（M3 optional）
        self.assertEqual(_check_payload(payload, "test_14"), [])
        # strict 模式 FAIL
        errs = _check_payload(payload, "test_14", strict_m3=True)
        self.assertTrue(
            any("'left_lanelet_id'" in e and "missing" in e for e in errs),
            f"strict mode should report missing left_lanelet_id: {errs}",
        )

    def test_15_strict_m3_full_payload_passes(self) -> None:
        """M3 strict mode: 全 10 字段齐 + 类型正确 → PASS."""
        errs = _check_payload(_m3_full_payload(), "test_15", strict_m3=True)
        self.assertEqual(errs, [], f"unexpected errors: {errs}")

    def test_16_flags_zero_is_valid(self) -> None:
        """M3: flags=0（ABI 保留位，当前未用）→ PASS.

        spec §6.1 v1.0 明文：flags 字段当前 producer 置 0，gate 必须放行.
        """
        payload = _m3_full_payload()
        payload["flags"] = 0
        errs = _check_payload(payload, "test_16")
        self.assertEqual(errs, [], f"unexpected errors: {errs}")
        # 也验证 strict 模式
        errs_strict = _check_payload(payload, "test_16", strict_m3=True)
        self.assertEqual(errs_strict, [], f"unexpected errors: {errs_strict}")

    def test_17_cli_strict_m3_via_main(self) -> None:
        """M3 CLI: --strict-m3 选项经 main() 传到 _check_payload."""
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            # 仅 M2 字段的 fixture
            m2_only = tmp / "m2_only.json"
            m2_only.write_text(json.dumps(_valid_payload()), encoding="utf-8")

            # 默认模式（无 --strict-m3）→ exit 0
            rc = gate_main([str(m2_only)])
            self.assertEqual(rc, 0, "M2-only should PASS in default mode")

            # --strict-m3 → exit 1（M3 字段缺失）
            rc = gate_main(["--strict-m3", str(m2_only)])
            self.assertEqual(rc, 1, "M2-only should FAIL under --strict-m3")

            # 全 10 字段 fixture 在 strict 下也 PASS
            full = tmp / "full.json"
            full.write_text(json.dumps(_m3_full_payload()), encoding="utf-8")
            rc = gate_main(["--strict-m3", str(full)])
            self.assertEqual(rc, 0, "full 10-field payload should PASS under --strict-m3")

            # --self-test 在 strict 模式下也 exit 0（内置 case 全 PASS）
            rc = gate_main(["--self-test", "--strict-m3"])
            self.assertEqual(rc, 0, "self-test --strict-m3 should exit 0")


if __name__ == "__main__":
    unittest.main()
