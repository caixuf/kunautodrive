#!/usr/bin/env python3
"""Behavior tests for demo_evaluator configuration-driven checks."""

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def load_evaluator():
    spec = importlib.util.spec_from_file_location("demo_evaluator", ROOT / "ci/evaluators/demo_evaluator.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DemoEvaluatorTest(unittest.TestCase):
    def test_road_network_projection_prefers_nearest_ramp_over_wider_road(self):
        evaluator = load_evaluator()
        scene = {
            "road_network": {
                "edges": [
                    {
                        "lanes": 1,
                        "lane_width": 3.5,
                        "nodes": [[0.0, 0.0], [100.0, 0.0]],
                    },
                    {
                        "lanes": 4,
                        "lane_width": 3.5,
                        "nodes": [[0.0, 10.0], [100.0, 10.0]],
                    },
                ],
            },
        }

        projection = evaluator._road_network_projection(scene, 50.0, 0.5)

        self.assertIsNotNone(projection)
        _, signed_offset, lane_count, _, _, _, edge_index = projection
        self.assertEqual(lane_count, 1)
        self.assertAlmostEqual(signed_offset, 0.5)
        self.assertEqual(edge_index, 0)

    def test_formal_metrics_report_tracking_comfort_and_timing_groups(self):
        evaluator = load_evaluator()
        samples = [
            {"t_demo": 0.0},
            {"t_demo": 0.1},
            {"t_demo": 0.2},
        ]
        series = [
            {"lane_error": 1.0, "speed": 0.0},
            {"lane_error": 0.5, "speed": 1.0},
            {"lane_error": 0.25, "speed": 2.0},
        ]
        metrics = evaluator.compute_formal_metrics(series, samples)
        self.assertEqual(metrics["trajectory_metric_type"], "closed_loop_lane_tracking")
        self.assertAlmostEqual(metrics["trajectory_ade_m"], 0.583333, places=4)
        self.assertAlmostEqual(metrics["trajectory_fde_m"], 0.25)
        self.assertAlmostEqual(metrics["comfort_accel_rms_mps2"], 10.0, places=4)
        self.assertEqual(metrics["timing_sample_count"], 3)
        self.assertAlmostEqual(metrics["timing_sample_period_mean_s"], 0.1)

    def test_formal_metrics_accept_dashboard_unix_second_timestamps(self):
        evaluator = load_evaluator()
        metrics = evaluator.compute_formal_metrics(
            [{"lane_error": 0.0, "speed": 0.0}] * 3,
            [
                {"timestamp": 1_700_000_000.0},
                {"timestamp": 1_700_000_000.1},
                {"timestamp": 1_700_000_000.2},
            ],
        )
        self.assertAlmostEqual(metrics["timing_sample_period_mean_s"], 0.1)

    def test_timestamp_delta_handles_microseconds_without_scaling_seconds(self):
        evaluator = load_evaluator()
        self.assertAlmostEqual(
            evaluator._timestamp_delta_seconds(1_700_000_000.1, 1_700_000_000.0),
            0.1,
            places=6,
        )

    def test_shadow_sidecar_uses_worker_workspace(self):
        evaluator = load_evaluator()
        with tempfile.TemporaryDirectory() as workspace:
            previous = os.environ.get("FLOWENGINE_TEMP_DIR")
            os.environ["FLOWENGINE_TEMP_DIR"] = workspace
            try:
                self.assertEqual(
                    evaluator._shadow_inference_files()[1],
                    Path(workspace) / "flow_tiny_inference.json",
                )
            finally:
                if previous is None:
                    os.environ.pop("FLOWENGINE_TEMP_DIR", None)
                else:
                    os.environ["FLOWENGINE_TEMP_DIR"] = previous
        self.assertAlmostEqual(
            evaluator._timestamp_delta_seconds(1_700_000_000_100_000.0,
                                               1_700_000_000_000_000.0),
            0.1,
            places=6,
        )

    def test_shadow_gate_requires_enough_settled_samples(self):
        evaluator = load_evaluator()
        with tempfile.TemporaryDirectory() as workspace:
            previous = os.environ.get("FLOWENGINE_TEMP_DIR")
            os.environ["FLOWENGINE_TEMP_DIR"] = workspace
            try:
                Path(workspace, "flow_tiny_inference.json").write_text(
                    '{"shadow_delta": -6.0, "shadow_speed_mae": 6.0, '
                    '"shadow_settled_n": 3, '
                    '"shadow_speed_mae_settled": 1.0}\n',
                    encoding="utf-8",
                )
                metrics = evaluator._load_shadow_metrics()
            finally:
                if previous is None:
                    os.environ.pop("FLOWENGINE_TEMP_DIR", None)
                else:
                    os.environ["FLOWENGINE_TEMP_DIR"] = previous

        self.assertFalse(metrics["gate_ready"])
        self.assertIsNone(metrics["mae"])
        self.assertEqual(metrics["settled_n"], 3)
        self.assertAlmostEqual(metrics["full_mae"], 6.0)

    def test_shadow_gate_uses_settled_mae_when_ready(self):
        evaluator = load_evaluator()
        with tempfile.TemporaryDirectory() as workspace:
            previous = os.environ.get("FLOWENGINE_TEMP_DIR")
            os.environ["FLOWENGINE_TEMP_DIR"] = workspace
            try:
                Path(workspace, "flow_tiny_inference.json").write_text(
                    '{"shadow_delta": -1.0, "shadow_speed_mae": 4.0, '
                    '"shadow_settled_n": 20, '
                    '"shadow_speed_mae_settled": 1.5}\n',
                    encoding="utf-8",
                )
                metrics = evaluator._load_shadow_metrics()
            finally:
                if previous is None:
                    os.environ.pop("FLOWENGINE_TEMP_DIR", None)
                else:
                    os.environ["FLOWENGINE_TEMP_DIR"] = previous

        self.assertTrue(metrics["gate_ready"])
        self.assertAlmostEqual(metrics["mae"], 1.5)
        self.assertAlmostEqual(metrics["full_mae"], 4.0)

    def test_shadow_gate_does_not_fail_without_settled_evidence(self):
        evaluator = load_evaluator()
        failures, warnings = evaluator._shadow_gate_issues({
            "delta": -6.0,
            "full_mae": 6.0,
            "mae": None,
            "settled_n": 0,
            "gate_ready": False,
        })
        self.assertEqual(failures, [])
        self.assertEqual(warnings, [])

        failures, warnings = evaluator._shadow_gate_issues({
            "delta": -6.0,
            "full_mae": 6.0,
            "mae": None,
            "settled_n": 3,
            "gate_ready": False,
        })
        self.assertEqual(failures, [])
        self.assertIn("inconclusive", warnings[0])

    def test_shadow_gate_skips_direct_control_output_contract(self):
        evaluator = load_evaluator()
        with tempfile.TemporaryDirectory() as workspace:
            previous = os.environ.get("FLOWENGINE_TEMP_DIR")
            os.environ["FLOWENGINE_TEMP_DIR"] = workspace
            try:
                Path(workspace, "flow_tiny_inference.json").write_text(
                    '{"shadow_delta": -9.0, "shadow_speed_mae": 9.0, '
                    '"shadow_settled_n": 20, '
                    '"shadow_speed_mae_settled": 9.0, '
                    '"prediction_contract": "direct_control", '
                    '"shadow_gate_supported": false}\n',
                    encoding="utf-8",
                )
                metrics = evaluator._load_shadow_metrics()
                failures, warnings = evaluator._shadow_gate_issues(metrics)
            finally:
                if previous is None:
                    os.environ.pop("FLOWENGINE_TEMP_DIR", None)
                else:
                    os.environ["FLOWENGINE_TEMP_DIR"] = previous

        self.assertFalse(metrics["gate_supported"])
        self.assertFalse(metrics["gate_ready"])
        self.assertIsNone(metrics["mae"])
        self.assertEqual(failures, [])
        self.assertIn("direct_control", warnings[0])

    def test_evaluation_payload_has_v1_envelope_and_legacy_summary(self):
        evaluator = load_evaluator()
        summary = {"scenario": "straight_road", "avg_speed_mps": 8.0}
        payload = evaluator.build_evaluation_payload(
            summary=summary,
            result="PASS",
            failures=[],
            warnings=["sample warning"],
            samples=[{"timestamp": 1.0}],
            npc_trajectories={"1": [{"t": 0.0, "x": 2.0}]},
            safety_evidence=None,
            scenario_file="scenarios/straight_road.json",
            mode="closed_loop",
            run_id="run-test-001",
        )

        self.assertEqual(payload["schema_version"], 1)
        self.assertEqual(payload["run"]["run_id"], "run-test-001")
        self.assertEqual(payload["run"]["scenario_id"], "straight_road")
        self.assertEqual(payload["run"]["mode"], "closed_loop")
        self.assertEqual(payload["metrics"], summary)
        self.assertIs(payload["summary"], summary)
        self.assertEqual(payload["npc_trajectories"]["1"][0]["x"], 2.0)

    def test_safety_evidence_contract_accepts_l3_emergency_stop(self):
        evaluator = load_evaluator()
        evidence = {
            "schema_version": 1,
            "fault": {
                "id": "raw_cmd_timeout",
                "type": "data_timeout",
                "component": "safety_control",
                "injected": True,
                "injected_at_us": 1_500_000,
                "detected_at_us": 3_500_001,
            },
            "degrade": {"level": 3, "reason": 9},
            "action": {
                "name": "emergency_stop",
                "immediate_stop": True,
                "command": {"throttle": 0.0, "brake": 1.0, "steer": 0.0},
            },
        }
        self.assertEqual(evaluator.validate_safety_evidence(evidence), [])

    def test_safety_evidence_contract_rejects_missing_brake(self):
        evaluator = load_evaluator()
        evidence = {
            "schema_version": 1,
            "fault": {
                "id": "raw_cmd_timeout",
                "type": "data_timeout",
                "component": "safety_control",
                "injected": True,
                "injected_at_us": 1,
                "detected_at_us": 2,
            },
            "degrade": {"level": 3},
            "action": {
                "name": "emergency_stop",
                "immediate_stop": True,
                "command": {"throttle": 0.0, "brake": 0.5},
            },
        }
        failures = evaluator.validate_safety_evidence(evidence)
        self.assertTrue(failures)
        self.assertIn("brake=1", failures[0])

    def test_expected_edges_are_generated_from_pipeline(self):
        evaluator = load_evaluator()
        pipeline = {
            "processes": [
                {"name": "producer", "publish": [{"topic": "topic/a", "type": "A"}]},
                {"name": "consumer", "subscribe": ["topic/a"]},
                {"name": "observer", "subscribe": ["topic/missing"]},
            ]
        }

        self.assertEqual(
            evaluator.expected_edges_from_pipeline(pipeline),
            [("producer", "topic/a", "consumer")],
        )

    def test_lane_change_requirement_is_scenario_criteria(self):
        evaluator = load_evaluator()
        sample = {
            "timestamp": 1.0,
            "metrics": {
                "topics": [
                    {"topic": "vehicle/state", "freq": 20.0},
                    {"topic": "sensor/lidar", "freq": 20.0},
                    {"topic": "sensor/gps", "freq": 10.0},
                    {"topic": "fusion/localization", "freq": 20.0},
                    {"topic": "planning/trajectory", "freq": 10.0},
                    {"topic": "control/raw_cmd", "freq": 10.0},
                    {"topic": "control/cmd", "freq": 10.0},
                ],
                "vehicle": {"speed": 10.0, "x": 100.0},
                "scene": {"ego": {"x": 100.0, "y": -1.75, "speed": 10.0}, "obstacles": []},
            },
            "nodes": [],
        }

        failures, _, _ = evaluator.score(
            [sample, sample],
            ROOT / "does-not-exist.log",
            criteria={"min_distance_m": 0.0, "min_avg_speed_mps": 0.0},
            expected_edges=[],
        )
        self.assertFalse(any("lane" in failure for failure in failures))

        failures, _, _ = evaluator.score(
            [sample, sample],
            ROOT / "does-not-exist.log",
            criteria={"required_lane_changes": 1, "min_distance_m": 0.0, "min_avg_speed_mps": 0.0},
            expected_edges=[],
        )
        self.assertTrue(any("lane changes too few" in failure for failure in failures))

    def test_perception_metrics_layered_recognition_rate(self):
        """Task 5: 分层识别率按 vehicle/vru 分层统计，漏检行人时 vru 识别率为 0。"""
        evaluator = load_evaluator()
        # 3 帧：每帧 2 car + 1 pedestrian 真值；perceived 仅匹配 car，漏检 ped
        series = []
        for _ in range(3):
            series.append({
                "x": 0.0, "speed": 10.0,
                "entities": [
                    {"id": 1, "type": "car", "x": 20.0, "y": -1.75},
                    {"id": 2, "type": "car", "x": 40.0, "y": 1.75},
                    {"id": 3, "type": "pedestrian", "x": 5.0, "y": 5.0},
                    {"id": 0, "type": "ego", "x": 0.0, "y": 0.0},
                ],
                # obs_world = 真值障碍（覆盖率分母）；perceived_world = 感知输出
                "obs_world": [
                    {"id": 1, "x": 20.0, "y": -1.75},
                    {"id": 2, "x": 40.0, "y": 1.75},
                    {"id": 3, "x": 5.0, "y": 5.0},
                ],
                "perceived_world": [
                    {"id": 100, "x": 20.5, "y": -1.75},   # 命中 car 1
                    {"id": 101, "x": 39.5, "y": 1.75},    # 命中 car 2
                    # 行人漏检
                ],
            })
        result = evaluator._compute_perception_metrics(series, [0.0, 1.0, 2.0])
        self.assertAlmostEqual(result["recognition_rate_vehicle"], 1.0)
        self.assertAlmostEqual(result["recognition_rate_vru"], 0.0)
        # 3 帧 × (2 car 命中 + 1 ped 漏) = 6 命中 / 9 真值 = 2/3
        self.assertAlmostEqual(result["recognition_rate_overall"], 2.0 / 3.0)
        self.assertEqual(result["truth_count_vehicle"], 6)
        self.assertEqual(result["truth_count_vru"], 3)
        self.assertEqual(result["truth_count_overall"], 9)
        # by_type 细粒度
        self.assertAlmostEqual(result["recognition_rate_by_type"]["car"], 1.0)
        self.assertAlmostEqual(result["recognition_rate_by_type"]["pedestrian"], 0.0)

    def test_recognition_uses_perception_output_not_truth(self):
        """识别率必须来自感知输出：真值有障碍但感知为空 → 0，而不是 1.0。

        2026-09-21 前的实现用 obs_world（monitor 从 vehicle/state 真值构造）当
        perceived，等于真值自比：ground_truth 与 sensor 模式跑出完全相同的
        recognition_rate_overall=1.000，感知链路坏了 CI 也不报。
        """
        evaluator = load_evaluator()
        series = [{
            "x": 0.0, "speed": 10.0,
            "entities": [{"id": 1, "type": "car", "x": 20.0, "y": 0.0},
                         {"id": 2, "type": "pedestrian", "x": 30.0, "y": 0.0}],
            "obs_world": [{"id": 1, "x": 20.0, "y": 0.0},
                          {"id": 2, "x": 30.0, "y": 0.0}],   # 真值两个都在
            "perceived_world": [{"id": 100, "x": 20.5, "y": 0.0}],  # 感知只看到车
        }]
        r = evaluator._compute_perception_metrics(series, [0.0])
        self.assertAlmostEqual(r["recognition_rate_vehicle"], 1.0)
        self.assertAlmostEqual(r["recognition_rate_vru"], 0.0)
        self.assertAlmostEqual(r["recognition_rate_overall"], 0.5)
        self.assertAlmostEqual(r["perception_coverage"], 1.0)
        self.assertAlmostEqual(r["perceived_count_avg"], 1.0)

        # 真值有、感知完全没有 → 覆盖率 0、识别率 0（旧实现这里会是 1.0）
        series[0]["perceived_world"] = []
        r2 = evaluator._compute_perception_metrics(series, [0.0])
        self.assertAlmostEqual(r2["perception_coverage"], 0.0)
        self.assertAlmostEqual(r2["recognition_rate_overall"], 0.0)

    def _write_pipeline(self, mode: str, prod_range_m: float, cons_range_m: float,
                        fov_deg: float = 120.0) -> Path:
        """写一份临时 pipeline 配置并返回路径（供 FLOW_PIPELINE 注入）。"""
        td = tempfile.mkdtemp()
        cfg = Path(td) / "pipeline_test.json"
        cfg.write_text(json.dumps({"processes": [
            {"name": "sensor_model", "params": json.dumps(
                {"lidar_mode": 1, "lidar_fov_deg": fov_deg,
                 "lidar_max_range_m": prod_range_m})},
            {"name": "perception", "params": json.dumps(
                {"mode": mode, "lidar_max_range_m": cons_range_m})},
        ]}))
        return cfg

    def _pin_pipeline(self, cfg: Path) -> None:
        prev = os.environ.get("FLOW_PIPELINE")
        os.environ["FLOW_PIPELINE"] = str(cfg)

        def restore():
            if prev is None:
                os.environ.pop("FLOW_PIPELINE", None)
            else:
                os.environ["FLOW_PIPELINE"] = prev

        self.addCleanup(restore)

    def test_recognition_denominator_excludes_unobservable_truth(self):
        """sensor 模式下锥外真值不计入分母：身后 / 超量程 / 出 FOV 都不算"漏检"。

        旧的"全部真值"分母把物理不可见判成漏检 —— 实测 urban_challenge 只有
        27.2% 的真值落在 ego 前向锥内，于是 sensor 模式无论多准都被压到 ~30% FAIL。
        """
        evaluator = load_evaluator()
        self._pin_pipeline(self._write_pipeline("sensor", 120.0, 120.0))
        series = [{
            "x": 0.0, "y": 0.0, "heading": 0.0, "speed": 10.0,
            "entities": [
                {"id": 1, "type": "car", "x": 50.0, "y": 0.0},     # 锥内（命中）
                {"id": 2, "type": "car", "x": -30.0, "y": 0.0},    # 身后
                {"id": 3, "type": "car", "x": 500.0, "y": 0.0},    # 超 120m 量程
                {"id": 4, "type": "car", "x": 0.0, "y": -5.0},     # 正侧方（±60° 外）
            ],
            "obs_world": [{"id": 1, "x": 50.0, "y": 0.0}],
            "perceived_world": [{"id": 100, "x": 50.5, "y": 0.0}],
        }]
        r = evaluator._compute_perception_metrics(series, [0.0])
        self.assertEqual(r["perception_mode"], "sensor")
        self.assertTrue(r["perception_observability_applied"])
        self.assertEqual(r["truth_count_overall"], 1)   # 只有正前方那辆
        self.assertAlmostEqual(r["recognition_rate_overall"], 1.0)
        self.assertAlmostEqual(r["perception_observable_ratio"], 0.25)
        self.assertAlmostEqual(r["perception_range_m"], 120.0)
        self.assertAlmostEqual(r["perception_fov_deg"], 120.0)

    def test_observability_cone_only_armed_in_sensor_mode(self):
        """ground_truth 模式**不做**锥内化：它的感知输入就是全部真值。

        无条件锥内化会把"actor 全程在视锥外"的场景（city_comprehensive /
        multi_light 各只声明 1~2 辆车）分母打成 0 → 门禁从"虚满分"翻成"无法判定"。
        """
        evaluator = load_evaluator()
        self._pin_pipeline(self._write_pipeline("ground_truth", 120.0, 120.0))
        series = [{
            "x": 0.0, "y": 0.0, "heading": 0.0, "speed": 10.0,
            "entities": [{"id": 1, "type": "car", "x": 500.0, "y": 0.0}],   # 锥外
            "obs_world": [{"id": 1, "x": 500.0, "y": 0.0}],
            "perceived_world": [{"id": 100, "x": 500.0, "y": 0.0}],
        }]
        r = evaluator._compute_perception_metrics(series, [0.0])
        self.assertEqual(r["perception_mode"], "ground_truth")
        self.assertFalse(r["perception_observability_applied"])
        self.assertEqual(r["truth_count_overall"], 1)          # 锥外也计入
        self.assertAlmostEqual(r["recognition_rate_overall"], 1.0)
        self.assertIsNone(r["perception_observable_ratio"])

    def test_recognition_falls_back_without_ego_pose(self):
        """sensor 模式但缺 ego y/heading → 退回旧口径，并标记未生效。

        不允许"静默退回旧口径"：`perception_observability_applied=False` 由
        score() 转成 WARN，否则度量退化无人知晓。
        """
        evaluator = load_evaluator()
        self._pin_pipeline(self._write_pipeline("sensor", 120.0, 120.0))
        series = [{
            "x": 0.0, "speed": 10.0,          # 故意不给 y / heading
            "entities": [{"id": 1, "type": "car", "x": 500.0, "y": 0.0}],
            "obs_world": [{"id": 1, "x": 500.0, "y": 0.0}],
            "perceived_world": [{"id": 100, "x": 500.0, "y": 0.0}],
        }]
        r = evaluator._compute_perception_metrics(series, [0.0])
        self.assertEqual(r["perception_mode"], "sensor")
        self.assertFalse(r["perception_observability_applied"])
        self.assertEqual(r["truth_count_overall"], 1)   # 500m 外也计入（旧口径）
        self.assertAlmostEqual(r["recognition_rate_overall"], 1.0)
        self.assertIsNone(r["perception_observable_ratio"])

    def test_pipeline_spec_takes_min_range_of_producer_and_consumer(self):
        """量程取生产者/消费者两处的 min：分歧时链路实际能看到的是较小者。

        `sensor_model.lidar_max_range_m` 与 `perception.lidar_max_range_m` 各有一份
        量程门（consumer 默认 60m 会把 producer 发的远处点静默丢光）。这里钉住
        "min" 这个语义，防止可观测性分母比链路真实视野更乐观。
        """
        evaluator = load_evaluator()
        cfg = self._write_pipeline("sensor", 120.0, 60.0, fov_deg=100.0)
        self._pin_pipeline(cfg)
        self.assertEqual(evaluator.pipeline_perception_spec(), (True, 100.0, 60.0))
        cfg2 = self._write_pipeline("ground_truth", 120.0, 120.0)
        self._pin_pipeline(cfg2)
        self.assertEqual(evaluator.pipeline_perception_spec(), (False, 120.0, 120.0))

    def test_coverage_denominator_is_cone_aware_in_sensor_mode(self):
        """覆盖率分母同样锥内化：锥外真值不算"该看见"，但锥内真值没输出仍要抓。"""
        evaluator = load_evaluator()
        self._pin_pipeline(self._write_pipeline("sensor", 120.0, 120.0))
        far = [{
            "x": 0.0, "y": 0.0, "heading": 0.0, "speed": 10.0,
            "entities": [{"id": 1, "type": "car", "x": 500.0, "y": 0.0}],   # 锥外
            "obs_world": [{"id": 1, "x": 500.0, "y": 0.0}],
            "perceived_world": [],
        }]
        r = evaluator._compute_perception_metrics(far, [0.0])
        self.assertAlmostEqual(r["perception_coverage"], 1.0)  # 分母为 0 → 约定 1.0

        near = [{
            "x": 0.0, "y": 0.0, "heading": 0.0, "speed": 10.0,
            "entities": [{"id": 1, "type": "car", "x": 30.0, "y": 0.0}],    # 锥内
            "obs_world": [{"id": 1, "x": 30.0, "y": 0.0}],
            "perceived_world": [],
        }]
        r2 = evaluator._compute_perception_metrics(near, [0.0])
        self.assertAlmostEqual(r2["perception_coverage"], 0.0)  # 真掉线，仍抓得住

        # 非 sensor 模式不受影响（旧口径）
        self._pin_pipeline(self._write_pipeline("ground_truth", 120.0, 120.0))
        r3 = evaluator._compute_perception_metrics(far, [0.0])
        self.assertAlmostEqual(r3["perception_coverage"], 0.0)

    def test_perception_metrics_warning_lead_time(self):
        """预警提前量 = TTC 跌破临界时刻 - 首次检测时刻，按**真值身份**跟踪。

        故意让 perception/obstacles 的 id 每帧递增（模拟 perception_points.c 的
        `frame_id*100+ci`）—— 跟踪键是真值实体身份，所以提前量照旧算得出来；用帧内
        局部 id 跟踪会恒 0（2026-09-22 sensor 模式 FAIL 的第一个根因）。
        """
        evaluator = load_evaluator()
        # 固定障碍 x=50；ego 从 x=0 匀速 10m/s，10 帧每秒 1 帧。
        # TTC < 3s 时 ego_x > 20 → 第 5 帧（ego_x=25, TTC=2.5）首次跌破。
        # 首次检测在第 0 帧 → warning_lead = 5.0s。
        series = []
        timestamps = []
        for i in range(10):
            series.append({
                "x": float(i * 5), "speed": 10.0,
                "entities": [{"id": 1, "type": "car", "x": 50.0, "y": 0.0}],
                "obs_world": [{"id": 1, "x": 50.0, "y": 0.0}],
                "perceived_world": [{"id": 100 * i, "x": 50.0, "y": 0.0}],  # 帧内局部 id
            })
            timestamps.append(float(i))
        result = evaluator._compute_perception_metrics(series, timestamps)
        self.assertEqual(result["critical_event_count"], 1)
        self.assertAlmostEqual(result["warning_lead_avg_s"], 5.0, places=2)
        self.assertAlmostEqual(result["warning_lead_min_s"], 5.0, places=2)
        # 末帧 ego_x=45, TTC=5/10=0.5
        self.assertAlmostEqual(result["min_ttc_s"], 0.5, places=2)

    def test_warning_lead_is_zero_when_truth_obstacle_never_detected(self):
        """真值障碍变成临界却从未被感知命中 → 提前量 0（"完全没预警"要 FAIL）。

        第一次检测必须来自"落在该真值匹配半径内"的感知输出；感知输出在别处
        （y=40 离真值 40m）不算命中，于是这个临界事件没有任何预警。
        """
        evaluator = load_evaluator()
        series = [{
            "x": float(i * 5), "speed": 10.0,
            "entities": [{"id": 1, "type": "car", "x": 50.0, "y": 0.0}],
            "obs_world": [{"id": 1, "x": 50.0, "y": 0.0}],
            "perceived_world": [{"id": 100 * i, "x": 50.0, "y": 40.0}],  # 不匹配真值
        } for i in range(10)]
        result = evaluator._compute_perception_metrics(
            series, [float(i) for i in range(10)])
        self.assertEqual(result["critical_event_count"], 1)
        self.assertAlmostEqual(result["warning_lead_min_s"], 0.0, places=2)

    def test_warning_lead_ignores_phantom_detections(self):
        """幻影感知（不与任何真值匹配）不产生 critical event。

        跟踪键是真值身份，所以"真值里不存在的那个障碍"不会凭空造出临界事件。
        （幻影导致幽灵刹车是**假阳性**问题，需要单独的正精度门禁，不在本指标范围。）
        """
        evaluator = load_evaluator()
        series = [{
            "x": float(i * 5), "speed": 10.0,
            "entities": [{"id": 1, "type": "car", "x": 500.0, "y": 0.0}],  # 远，永不临界
            "obs_world": [{"id": 1, "x": 500.0, "y": 0.0}],
            "perceived_world": [{"id": 100 * i, "x": 50.0, "y": 0.0}],     # 真值里没有
        } for i in range(10)]
        result = evaluator._compute_perception_metrics(
            series, [float(i) for i in range(10)])
        self.assertEqual(result["critical_event_count"], 0)

    def test_perception_metrics_no_critical_event(self):
        """无临界事件时 critical_event_count=0，不抛异常。"""
        evaluator = load_evaluator()
        series = [{
            "x": 0.0, "speed": 1.0,
            "entities": [{"id": 1, "type": "car", "x": 100.0, "y": 0.0}],
            "obs_world": [{"id": 1, "x": 100.0, "y": 0.0}],
            "perceived_world": [{"id": 100, "x": 100.0, "y": 0.0}],
        }]
        result = evaluator._compute_perception_metrics(series, [0.0])
        self.assertEqual(result["critical_event_count"], 0)
        self.assertEqual(result["warning_lead_avg_s"], 0.0)
        self.assertEqual(result["warning_lead_min_s"], 0.0)
        # TTC = 100/1 = 100s，远大于临界 3s，min_ttc_s 应为 100
        self.assertAlmostEqual(result["min_ttc_s"], 100.0, places=2)

    def test_sample_metrics_uses_road_network_polyline_for_curved_roads(self):
        evaluator = load_evaluator()
        sample = {
            "metrics": {
                "vehicle": {"speed": 3.0, "x": 206.25},
                "scene": {
                    "ego": {"x": 206.25, "y": -54.0, "heading": 0.0, "speed": 3.0},
                    "lane": {"width": 3.5, "count": 2, "center": 0.0},
                    "obstacles": [],
                    "road_network": {
                        "edges": [
                            {
                                "id": 2,
                                "lanes": 2,
                                "lane_width": 3.5,
                                "nodes": [
                                    [208.0, -28.0, 0.0],
                                    [208.0, -80.0, 0.0],
                                ],
                            }
                        ]
                    },
                },
            }
        }

        metrics = evaluator.sample_metrics(sample, road=None)

        self.assertLess(metrics["lane_error"], 0.2)
        self.assertGreater(metrics["road_edge_margin"], 0.7)

    def test_sample_metrics_marks_lane_change_as_maneuver_window(self):
        evaluator = load_evaluator()
        sample = {
            "metrics": {
                "vehicle": {"speed": 10.0, "x": 0.0},
                "scene": {
                    "ego": {"x": 0.0, "y": 0.0, "heading": 0.0},
                    "lane": {"width": 3.5, "count": 2},
                    "obstacles": [],
                },
                "behavior": {"state": "LEFT_CHANGE"},
            }
        }

        metrics = evaluator.sample_metrics(sample, road=None)

        self.assertEqual(metrics["behavior_state"], "LEFT_CHANGE")
        self.assertTrue(metrics["maneuver_active"])


    def test_pipeline_declares_behavior_planner_flowsim_edges(self):
        """Regression: behavior_planner must subscribe to vehicle/state and scene/frame
        (both published by flowsim).  Missing these in config/pipeline.json or in the
        s_inputs metadata causes the evaluator's topology check to emit:
            missing topology edge flowsim --vehicle/state--> behavior_planner
            missing topology edge flowsim --scene/frame--> behavior_planner
        This test catches the config-level half of that regression (CI run 31196721920).
        """
        import json

        evaluator = load_evaluator()
        pipeline_path = ROOT / "config" / "pipeline.json"
        with pipeline_path.open(encoding="utf-8") as f:
            pipeline = json.load(f)

        edges = evaluator.expected_edges_from_pipeline(pipeline)
        edge_set = {(pub, topic, sub) for pub, topic, sub in edges}

        self.assertIn(
            ("flowsim", "vehicle/state", "behavior_planner"),
            edge_set,
            "pipeline.json must declare flowsim publishing vehicle/state "
            "and behavior_planner subscribing to it",
        )
        self.assertIn(
            ("flowsim", "scene/frame", "behavior_planner"),
            edge_set,
            "pipeline.json must declare flowsim publishing scene/frame "
            "and behavior_planner subscribing to it",
        )

    def test_shadow_sidecar_loads_extended_metrics(self):
        evaluator = load_evaluator()
        with tempfile.TemporaryDirectory() as workspace:
            previous = os.environ.get("FLOWENGINE_TEMP_DIR")
            os.environ["FLOWENGINE_TEMP_DIR"] = workspace
            try:
                Path(workspace, "flow_tiny_inference.json").write_text(
                    '{"shadow_delta": -0.5, "shadow_speed_mae": 1.2, '
                    '"shadow_settled_n": 25, '
                    '"shadow_speed_mae_settled": 0.8, '
                    '"shadow_steer_mae": 0.035, '
                    '"shadow_steer_rmse": 0.048, '
                    '"shadow_ade": 0.42, '
                    '"shadow_fde": 0.85}\n',
                    encoding="utf-8",
                )
                metrics = evaluator._load_shadow_metrics()
            finally:
                if previous is None:
                    os.environ.pop("FLOWENGINE_TEMP_DIR", None)
                else:
                    os.environ["FLOWENGINE_TEMP_DIR"] = previous

        self.assertTrue(metrics["gate_ready"])
        self.assertAlmostEqual(metrics["mae"], 0.8)
        self.assertAlmostEqual(metrics["steer_mae"], 0.035)
        self.assertAlmostEqual(metrics["steer_rmse"], 0.048)
        self.assertAlmostEqual(metrics["ade"], 0.42)
        self.assertAlmostEqual(metrics["fde"], 0.85)

    def test_lateral_excursion_monotonic_lane_changes_not_snaking(self):
        """W1: 单向多车道合法连续变道（y 范围 >4.5m 但方向单调）应 WARN 而非 FAIL；
        来回翻转的蛇形仍 FAIL。"""
        evaluator = load_evaluator()

        def make_sample(y: float):
            return {
                "timestamp": 1.0,
                "metrics": {
                    "topics": [{"topic": "vehicle/state", "freq": 20.0}],
                    "vehicle": {"speed": 10.0, "x": 100.0},
                    "scene": {"ego": {"x": 100.0, "y": y, "speed": 10.0}, "obstacles": []},
                },
                "nodes": [],
            }

        # 单调连续变道：-1.75 → 5.25（跨 3 个车道，单向 4 车道合法超车），方向不翻转。
        mono = [make_sample(-1.75 + 0.7 * i) for i in range(11)]
        failures, warnings, _ = evaluator.score(
            mono,
            ROOT / "does-not-exist.log",
            criteria={"min_avg_speed_mps": 0.0},
            expected_edges=[],
        )
        self.assertFalse(
            any("lateral excursion too large" in f for f in failures),
            f"monotonic multi-lane change should not fail, got {failures}",
        )
        self.assertTrue(
            any("lateral excursion" in w for w in warnings),
            f"expected lateral excursion WARN, got warnings={warnings}",
        )

        # 蛇形：-1.75 ↔ 5.25 来回翻转（≥3 次方向翻转），应 FAIL。
        snake = [make_sample(-1.75 if i % 2 == 0 else 5.25) for i in range(11)]
        failures, _, _ = evaluator.score(
            snake,
            ROOT / "does-not-exist.log",
            criteria={"min_avg_speed_mps": 0.0},
            expected_edges=[],
        )
        self.assertTrue(
            any("lateral excursion too large" in f for f in failures),
            f"snaking across lanes should fail, got failures={failures}",
        )

    def _behavior_samples(self, states: list) -> list:
        """构造带 metrics.behavior 的样本（模拟 monitor 透传的 behavior/state）。"""
        out = []
        for i, state in enumerate(states):
            out.append({
                "timestamp": 1.0 + 0.25 * i,
                "metrics": {
                    "topics": [{"topic": "vehicle/state", "freq": 20.0}],
                    "vehicle": {"speed": 10.0, "x": 100.0 + 10.0 * i},
                    "scene": {"ego": {"x": 100.0 + 10.0 * i, "y": -1.75, "speed": 10.0},
                              "obstacles": []},
                    "behavior": {"state": state, "committed_lane": 2, "obs_count": 0},
                },
                "nodes": [],
            })
        return out

    def test_behavior_silent_for_whole_run_fails(self):
        """B: behavior/state 全程 NA = 决策块被跳过（空障碍物列表被当"没就绪"）。

        2026-09-23 实测：sensor 编排 straight_road 掉头触发被跳过 30s，车冲出
        路面；既有量活性门禁全绿（车仍在动）→ 必须由这条判据抓住。
        """
        evaluator = load_evaluator()
        samples = self._behavior_samples(["NA"] * 8)
        failures, _, _ = evaluator.score(
            samples, ROOT / "does-not-exist.log",
            criteria={"min_avg_speed_mps": 0.0}, expected_edges=[],
        )
        self.assertTrue(
            any("behavior planner never decided" in f for f in failures),
            f"all-NA behavior state must FAIL, got {failures}",
        )

    def test_behavior_silent_half_run_warns_not_fails(self):
        """半数以上样本 NA → WARN（启动慢/上游稀疏不足以阻断），不得 FAIL。"""
        evaluator = load_evaluator()
        samples = self._behavior_samples(["NA"] * 6 + ["CRUISE"] * 4)
        failures, warnings, _ = evaluator.score(
            samples, ROOT / "does-not-exist.log",
            criteria={"min_avg_speed_mps": 0.0}, expected_edges=[],
        )
        self.assertFalse(
            any("behavior planner never decided" in f for f in failures),
            f"partial silence must not FAIL, got {failures}",
        )
        self.assertTrue(
            any("behavior planner silent" in w for w in warnings),
            f"partial silence should WARN, got {warnings}",
        )

    def test_behavior_gate_skipped_without_behavior_segment(self):
        """样本不带 metrics.behavior（单测构造/无 behavior 的 profile）→ 判据静默跳过。"""
        evaluator = load_evaluator()
        samples = [{
            "timestamp": 1.0 + 0.25 * i,
            "metrics": {
                "topics": [{"topic": "vehicle/state", "freq": 20.0}],
                "vehicle": {"speed": 10.0, "x": 100.0 + 10.0 * i},
                "scene": {"ego": {"x": 100.0 + 10.0 * i, "y": -1.75, "speed": 10.0},
                          "obstacles": []},
            },
            "nodes": [],
        } for i in range(8)]
        failures, warnings, _ = evaluator.score(
            samples, ROOT / "does-not-exist.log",
            criteria={"min_avg_speed_mps": 0.0}, expected_edges=[],
        )
        self.assertFalse(
            any("behavior planner" in f for f in failures + warnings),
            f"absent behavior segment must not trip the gate, got {failures} {warnings}",
        )


    def _gap_samples(self, speed: float, rel_x: float, n: int = 8) -> list:
        """构造"前方同车道有车"的样本：min_forward_gap = rel_x − 4.6（无路网时走 rel 分支）。"""
        return [{
            "timestamp": 1.0 + 0.25 * i,
            "metrics": {
                "topics": [{"topic": "vehicle/state", "freq": 20.0}],
                "vehicle": {"speed": speed, "x": 100.0 + speed * 0.25 * i},
                "scene": {
                    "ego": {"x": 100.0 + speed * 0.25 * i, "y": -1.75, "speed": speed},
                    "obstacles": [{"id": 1, "x": rel_x, "y": 0.0, "len": 4.6, "wid": 2.0}],
                },
            },
            "nodes": [],
        } for i in range(n)]

    def test_gap_criterion_uses_per_frame_speed(self):
        """跟车判据必须与**该帧车速**的期望间距比，而不是整段中位速度。

        2026-09-23 实测（lane_change_traffic）：最差帧 gap=4.96m @ ego 1.0 m/s
        （该速度下期望 6.5m，安全），却被 v_med=9.0 的期望间距 18.5m 判 FAIL；
        同一场景另一趟 run 全程没被压到低速（v_med 16.99）就 PASS —— 结论在
        "这一趟有没有低速段"之间摇摆。逐帧判据下三次实测 0 帧违规、28 帧误报消失。
        """
        evaluator = load_evaluator()
        crit = {"min_avg_speed_mps": 0.0}

        # A. 低速逼近（排队/让行语义）：不得 FAIL
        failures, _, _ = evaluator.score(
            self._gap_samples(1.0, 9.56), ROOT / "does-not-exist.log",
            criteria=crit, expected_edges=[],
        )
        self.assertFalse(
            any("min_forward_gap" in f for f in failures),
            f"creeping at 1.0 m/s behind a lead must not FAIL, got {failures}",
        )

        # B. 高速贴近：必须仍然 FAIL（判据不能因为逐帧化就失去牙）
        failures, _, _ = evaluator.score(
            self._gap_samples(20.0, 14.6), ROOT / "does-not-exist.log",
            criteria=crit, expected_edges=[],
        )
        self.assertTrue(
            any("min_forward_gap" in f for f in failures),
            f"10m gap at 20 m/s (desired 35m) must FAIL, got {failures}",
        )

        # C. 静止排队：gap 3.0m @ 0 m/s（不能算追尾风险）
        failures, _, _ = evaluator.score(
            self._gap_samples(0.0, 7.6), ROOT / "does-not-exist.log",
            criteria=crit, expected_edges=[],
        )
        self.assertFalse(
            any("min_forward_gap" in f for f in failures),
            f"stopped 3m behind a stopped car must not FAIL, got {failures}",
        )

        # D. 真追尾（gap <= 0）在任何速度下都必须 FAIL
        failures, _, _ = evaluator.score(
            self._gap_samples(0.0, 3.0), ROOT / "does-not-exist.log",
            criteria=crit, expected_edges=[],
        )
        self.assertTrue(
            any("rear-end collision risk" in f for f in failures),
            f"gap<=0 must FAIL, got {failures}",
        )

    def _duration_samples(self, wall_span: float, sim_span: float, n: int = 12) -> list:
        """构造带**仿真钟**的样本：墙钟跨度 wall_span、仿真跨度 sim_span。"""
        out = []
        for i in range(n):
            frac = i / (n - 1)
            wall = 1000.0 + wall_span * frac
            sim_us = 5_000_000 + sim_span * 1e6 * frac
            out.append({
                "timestamp": wall,
                "metrics": {
                    "topics": [{"topic": "vehicle/state", "freq": 20.0}],
                    "vehicle": {"speed": 12.0, "x": 10.0 + 10.0 * i},
                    "scene": {
                        "t_us": sim_us,
                        "ego": {"x": 10.0 + 10.0 * i, "y": -1.75, "speed": 12.0},
                        "obstacles": [],
                    },
                },
                "nodes": [],
            })
        return out

    def test_max_duration_judged_on_simulation_time(self):
        """max_duration_s 必须按**仿真钟**判，不能按墙钟跨度。

        2026-09-23 实测 curve_road：墙钟 63.3s（含 demo.sh 启动/收尾与机器负载）
        而仿真只有 59.3s（场景声明 60s）→ 被 +1.0s 容差反复误判
        "exceeded max duration"；A/B 证明与算法改动无关，是度量伪影。
        """
        evaluator = load_evaluator()
        crit = {"min_avg_speed_mps": 0.0, "max_duration_s": 60.0}

        # A. 墙钟超、仿真没超 → 不得 FAIL（应为 WARN）
        failures, warnings, _ = evaluator.score(
            self._duration_samples(63.3, 59.3), ROOT / "does-not-exist.log",
            criteria=crit, expected_edges=[],
        )
        self.assertFalse(
            any("exceeded max duration" in f for f in failures),
            f"wall-clock overshoot with healthy sim time must not FAIL, got {failures}",
        )
        self.assertTrue(
            any("wall-clock span" in w for w in warnings),
            f"wall-clock overshoot should stay visible as WARN, got {warnings}",
        )

        # B. 仿真钟真超了 → 必须 FAIL（真挂死不能被放过）
        failures, _, _ = evaluator.score(
            self._duration_samples(95.0, 92.0), ROOT / "does-not-exist.log",
            criteria=crit, expected_edges=[],
        )
        self.assertTrue(
            any("exceeded max duration" in f for f in failures),
            f"simulation overrun must FAIL, got {failures}",
        )

        # C. 没有仿真钟（老 series）→ 回退墙钟，语义不变
        plain = self._duration_samples(70.0, 68.0)
        for s in plain:
            del s["metrics"]["scene"]["t_us"]
        failures, _, _ = evaluator.score(
            plain, ROOT / "does-not-exist.log", criteria=crit, expected_edges=[],
        )
        self.assertTrue(
            any("exceeded max duration" in f for f in failures),
            f"without sim clock the wall-clock check must still apply, got {failures}",
        )


if __name__ == "__main__":
    unittest.main()
