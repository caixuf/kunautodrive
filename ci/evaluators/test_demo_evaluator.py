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

    def test_perception_metrics_warning_lead_time(self):
        """Task 5: 预警提前量 = TTC 跌破临界时刻 - 首次检测时刻。"""
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
                "perceived_world": [{"id": 100, "x": 50.0, "y": 0.0}],
            })
            timestamps.append(float(i))
        result = evaluator._compute_perception_metrics(series, timestamps)
        self.assertEqual(result["critical_event_count"], 1)
        self.assertAlmostEqual(result["warning_lead_avg_s"], 5.0, places=2)
        self.assertAlmostEqual(result["warning_lead_min_s"], 5.0, places=2)
        # 末帧 ego_x=45, TTC=5/10=0.5
        self.assertAlmostEqual(result["min_ttc_s"], 0.5, places=2)

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


if __name__ == "__main__":
    unittest.main()
