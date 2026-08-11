#!/usr/bin/env python3
"""Aggregate versioned evaluation artifacts without rerunning simulation.

The report consumes existing ``demo_evaluator.py`` JSON artifacts or an
optional prediction artifact.  Open-loop ADE/FDE are computed only when a
record contains both equal-length ``prediction`` and ``ground_truth``
trajectories.  MPI is passed through only when the producer supplies a numeric
``mpi`` value and an optional ``mpi_definition``; this tool never invents a
definition for a missing metric.
"""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any, Iterable

REPORT_SCHEMA = "flowengine.evaluation_report.v1"


def _point(value: Any, label: str) -> tuple[float, float]:
    if isinstance(value, dict):
        x, y = value.get("x"), value.get("y")
    elif isinstance(value, (list, tuple)) and len(value) >= 2:
        x, y = value[0], value[1]
    else:
        raise ValueError(f"{label} must be an object with x/y or a [x, y] pair")
    if not isinstance(x, (int, float)) or isinstance(x, bool) or \
            not isinstance(y, (int, float)) or isinstance(y, bool):
        raise ValueError(f"{label} coordinates must be numeric")
    if not math.isfinite(float(x)) or not math.isfinite(float(y)):
        raise ValueError(f"{label} coordinates must be finite")
    return float(x), float(y)


def trajectory_error(prediction: Any, ground_truth: Any, label: str = "trajectory") -> tuple[float, float]:
    """Return standard ADE/FDE for one equal-horizon 2D trajectory pair."""
    if not isinstance(prediction, list) or not isinstance(ground_truth, list):
        raise ValueError(f"{label} prediction and ground_truth must be arrays")
    if not prediction or len(prediction) != len(ground_truth):
        raise ValueError(f"{label} prediction and ground_truth must have equal non-zero length")
    distances = []
    for index, (predicted, actual) in enumerate(zip(prediction, ground_truth)):
        px, py = _point(predicted, f"{label}.prediction[{index}]")
        gx, gy = _point(actual, f"{label}.ground_truth[{index}]")
        distances.append(math.hypot(px - gx, py - gy))
    return statistics.fmean(distances), distances[-1]


def _trajectory_records(value: Any) -> list[dict]:
    if isinstance(value, dict):
        records = value.get("predictions")
        if isinstance(records, list):
            return [item for item in records if isinstance(item, dict)]
        if "prediction" in value or "predicted_trajectory" in value:
            return [value]
        samples = value.get("samples")
        if isinstance(samples, list):
            return [item for item in samples if isinstance(item, dict)]
    if isinstance(value, list):
        return [item for item in value if isinstance(item, dict)]
    return []


def compute_open_loop_metrics(records: Iterable[dict]) -> dict:
    """Compute macro ADE/FDE and surface malformed prediction records."""
    ades: list[float] = []
    fdes: list[float] = []
    errors: list[str] = []
    record_count = 0
    for index, record in enumerate(records):
        prediction = record.get("prediction", record.get("predicted_trajectory"))
        ground_truth = record.get("ground_truth", record.get("ground_truth_trajectory"))
        if prediction is None and ground_truth is None:
            continue
        record_count += 1
        try:
            ade, fde = trajectory_error(prediction, ground_truth, f"record[{index}]")
        except ValueError as exc:
            errors.append(str(exc))
            continue
        ades.append(ade)
        fdes.append(fde)
    if errors:
        return {
            "status": "invalid",
            "trajectory_count": record_count,
            "ade_m": None,
            "fde_m": None,
            "errors": errors,
        }
    if not ades:
        return {
            "status": "unavailable",
            "trajectory_count": 0,
            "ade_m": None,
            "fde_m": None,
            "errors": [],
        }
    return {
        "status": "computed",
        "trajectory_count": len(ades),
        "ade_m": statistics.fmean(ades),
        "fde_m": statistics.fmean(fdes),
        "errors": [],
    }


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"{path}: cannot read JSON: {exc}") from exc


def _result_payloads(input_path: Path) -> list[dict]:
    if input_path.is_file():
        value = _load_json(input_path)
        return [value] if isinstance(value, dict) else []
    if not input_path.is_dir():
        raise ValueError(f"{input_path}: input path does not exist")
    manifest_path = input_path / "run_manifest.json"
    if manifest_path.is_file():
        manifest = _load_json(manifest_path)
        rows = manifest.get("results", []) if isinstance(manifest, dict) else []
        if not isinstance(rows, list):
            raise ValueError(f"{manifest_path}: results must be an array")
        result_paths = [
            Path(row["result_path"]) if Path(row["result_path"]).is_absolute()
            else input_path / Path(row["result_path"]).name
            for row in rows
            if isinstance(row, dict) and isinstance(row.get("result_path"), str)
        ]
    else:
        result_paths = sorted(input_path.glob("*.json"))
    payloads = []
    for path in result_paths:
        if path.name == "run_manifest.json" or not path.is_file():
            continue
        value = _load_json(path)
        if isinstance(value, dict):
            payloads.append(value)
    return payloads


def _mpi_summary(payloads: Iterable[dict]) -> dict:
    values: list[float] = []
    definitions: set[str] = set()
    for payload in payloads:
        metrics = payload.get("metrics", {})
        if not isinstance(metrics, dict):
            continue
        value = metrics.get("mpi")
        if isinstance(value, (int, float)) and not isinstance(value, bool) and \
                math.isfinite(float(value)):
            definition = metrics.get("mpi_definition")
            if not isinstance(definition, str) or not definition:
                return {
                    "status": "invalid",
                    "mean": None,
                    "definition": None,
                    "error": "numeric mpi requires mpi_definition",
                }
            values.append(float(value))
            definitions.add(definition)
    if not values:
        return {"status": "unavailable", "mean": None, "definition": None, "error": None}
    if len(definitions) > 1:
        return {
            "status": "invalid",
            "mean": None,
            "definition": None,
            "error": "multiple mpi_definition values cannot be aggregated",
        }
    return {
        "status": "computed",
        "mean": statistics.fmean(values),
        "definition": next(iter(definitions), None),
        "error": None,
    }


def build_report(payloads: list[dict], prediction_value: Any = None) -> dict:
    scenario_ids = []
    pass_count = 0
    trajectory_records: list[dict] = []
    for payload in payloads:
        scenario = payload.get("scenario") or payload.get("run", {}).get("scenario_id")
        if scenario:
            scenario_ids.append(str(scenario))
        if payload.get("result") == "PASS":
            pass_count += 1
        trajectory_records.extend(_trajectory_records(payload))

    if prediction_value is not None:
        trajectory_records.extend(_trajectory_records(prediction_value))
    open_loop = compute_open_loop_metrics(trajectory_records)
    scenario_count = len(payloads)
    return {
        "schema_version": REPORT_SCHEMA,
        "scenario_count": scenario_count,
        "pass_count": pass_count,
        "scenario_pass_rate": pass_count / scenario_count if scenario_count else None,
        "scenario_pass_rate_status": "computed" if scenario_count else "unavailable",
        "open_loop": open_loop,
        "mpi": _mpi_summary(payloads),
        "scenarios": scenario_ids,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True,
                        help="one evaluator JSON or a results directory")
    parser.add_argument("--predictions", type=Path,
                        help="optional JSON with predictions and ground truth")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    try:
        payloads = _result_payloads(args.input)
        predictions = _load_json(args.predictions) if args.predictions else None
        report = build_report(payloads, predictions)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    if args.as_json:
        print(json.dumps(report, indent=2, ensure_ascii=False))
    else:
        print(f"scenario pass rate: {report['scenario_pass_rate']}")
        print(f"open-loop ADE/FDE: {report['open_loop']['status']} "
              f"{report['open_loop']['ade_m']}/{report['open_loop']['fde_m']}")
        print(f"MPI: {report['mpi']['status']} {report['mpi']['mean']}")
    return 0 if report["open_loop"]["status"] != "invalid" and \
        report["mpi"]["status"] != "invalid" else 2


if __name__ == "__main__":
    sys.exit(main())
