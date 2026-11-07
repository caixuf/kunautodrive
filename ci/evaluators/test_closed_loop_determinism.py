#!/usr/bin/env python3
"""方向五：闭环评估的确定性回归。

背景：方向五要求"确定性回放 + 指标评估"作为模型迭代的放行条件。真实 demo 的
影子评估依赖墙钟与线程调度，同种子不保证同轨迹（见 docs/FAILURE_MODES.md）；
而 `tools/train_e2e/eval_closed_loop.py` 是纯 Python 自行车模型 + 固定 DT、
无随机源，**天然可复现** —— 它是目前唯一能拿来做确定性断言的评估口径。

本测试锁定该性质：同一模型跑两遍，除 `created_unix_ms` 外，
metrics / gates / result 必须逐字段相等，且任何指标不得是 NaN/Inf。
一旦有人给闭环评估引入时间戳参与计算、随机初始化或浮点非确定性，
这里会先红，而不是等 CI 的涌现指标偶发波动。
"""

from __future__ import annotations

import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EVAL = ROOT / "tools" / "train_e2e" / "eval_closed_loop.py"
# 仓库内已入库、无需训练即可重复推理的 tiny-MLP artifact。
MODEL = ROOT / "tools" / "train" / "model.txt"


def _strip_volatile(doc: dict) -> dict:
    """去掉唯一的非确定性字段（墙钟时间戳），其余必须逐字段一致。"""
    out = json.loads(json.dumps(doc))  # deep copy
    out.pop("created_unix_ms", None)
    return out


def _assert_finite(value, path: str = "") -> None:
    if isinstance(value, float):
        if math.isnan(value) or math.isinf(value):
            raise AssertionError(f"non-finite metric at {path}: {value!r}")
    elif isinstance(value, dict):
        for k, v in value.items():
            _assert_finite(v, f"{path}.{k}")
    elif isinstance(value, list):
        for i, v in enumerate(value):
            _assert_finite(v, f"{path}[{i}]")


class ClosedLoopDeterminismTest(unittest.TestCase):
    def _run(self, output: Path) -> dict:
        result = subprocess.run(
            [sys.executable, str(EVAL), "--model", str(MODEL),
             "--output", str(output), "--json"],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        # 评测结论本身可能是 FAIL（仓库内 artifact 不代表当前场景集都过），
        # 本测试只关心**确定性**：只要产物文件生成且可解析即可。
        self.assertTrue(
            output.is_file(),
            f"closed-loop evaluator produced no output:\n{result.stdout}\n{result.stderr}",
        )
        return json.loads(output.read_text(encoding="utf-8"))

    def test_same_model_twice_is_metric_identical(self):
        self.assertTrue(MODEL.is_file(), f"missing in-repo artifact: {MODEL}")
        with tempfile.TemporaryDirectory() as tmp:
            first = self._run(Path(tmp) / "run_a.json")
            second = self._run(Path(tmp) / "run_b.json")

            _assert_finite(first, "run_a")
            _assert_finite(second, "run_b")

            self.assertEqual(
                _strip_volatile(first), _strip_volatile(second),
                "closed-loop evaluation must be deterministic for the same model",
            )

            # 场景覆盖与判定集合也必须一致（防止某次静默少跑一个场景）
            self.assertEqual(set(first.get("scenarios", {})),
                             set(second.get("scenarios", {})))
            for name in first.get("scenarios", {}):
                a = first["scenarios"][name]
                b = second["scenarios"][name]
                self.assertEqual(a.get("result"), b.get("result"),
                                 f"scenario {name} verdict drifted between runs")
                self.assertEqual(a.get("gates"), b.get("gates"),
                                 f"scenario {name} gates drifted between runs")
                self.assertEqual(a.get("metrics"), b.get("metrics"),
                                 f"scenario {name} metrics drifted between runs")

    def test_created_unix_ms_is_the_only_volatile_field(self):
        """显式锁定"只允许时间戳变"这一契约，避免以后有人把 datetime 混进 metrics。"""
        with tempfile.TemporaryDirectory() as tmp:
            first = self._run(Path(tmp) / "a.json")
            second = self._run(Path(tmp) / "b.json")
        volatile = {
            k for k in set(first) | set(second)
            if first.get(k) != second.get(k)
        }
        self.assertLessEqual(volatile, {"created_unix_ms"},
                             f"unexpected volatile fields: {volatile}")


if __name__ == "__main__":
    unittest.main()