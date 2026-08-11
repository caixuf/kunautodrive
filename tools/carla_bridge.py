#!/usr/bin/env python3
"""Capability-gated CARLA adapter boundary for FlowEngine evaluation.

The bridge deliberately keeps CARLA optional.  ``capabilities`` is safe to run
on every developer/CI machine; ``check`` exits non-zero when the CARLA Python
API is unavailable instead of silently falling back to FlowSim.

The adapter owns only simulator I/O:

* CARLA clock snapshots are converted to ``SimulationClock``;
* sensor callbacks are buffered as timestamped ``SensorPacket`` objects;
* FlowEngine ``ControlCommand`` values are mapped to ``carla.VehicleControl``.

Planning, safety decisions, and evaluation remain in the existing FlowEngine
pipeline and evaluator.
"""

from __future__ import annotations

import argparse
import importlib
import json
import math
import sys
from collections import deque
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Deque

ROOT = Path(__file__).resolve().parents[1]
CAPABILITY_SCHEMA = "flowengine.simulator_capabilities.v1"


class CarlaUnavailableError(RuntimeError):
    """Raised when the optional CARLA Python API cannot be loaded."""


@dataclass(frozen=True)
class SimulationClock:
    frame: int
    sim_time_s: float
    delta_s: float


@dataclass(frozen=True)
class ControlCommand:
    throttle: float = 0.0
    brake: float = 0.0
    steer: float = 0.0
    gear: int = 0
    hand_brake: bool = False


@dataclass(frozen=True)
class SensorSpec:
    name: str
    sensor_type: str
    frame: str
    timestamp_domain: str = "simulation"
    required: bool = False


@dataclass(frozen=True)
class SensorPacket:
    name: str
    frame: int
    timestamp_s: float
    payload: Any


@dataclass
class SensorBatch:
    clock: SimulationClock
    packets: dict[str, SensorPacket] = field(default_factory=dict)


def _clamp(value: float, low: float, high: float, label: str) -> float:
    if not math.isfinite(value):
        raise ValueError(f"{label} must be finite")
    return max(low, min(high, value))


def normalize_control(command: ControlCommand) -> ControlCommand:
    """Validate and clamp the transport-neutral FlowEngine control contract."""
    if not isinstance(command.gear, int):
        raise ValueError("gear must be an integer")
    return ControlCommand(
        throttle=_clamp(float(command.throttle), 0.0, 1.0, "throttle"),
        brake=_clamp(float(command.brake), 0.0, 1.0, "brake"),
        steer=_clamp(float(command.steer), -1.0, 1.0, "steer"),
        gear=command.gear,
        hand_brake=bool(command.hand_brake),
    )


def _carla_spec() -> dict[str, Any]:
    reason = "CARLA Python API is not installed"
    try:
        importlib.import_module("carla")
    except (ImportError, OSError):
        available = False
    else:
        available = True
        reason = None
    return {
        "schema": CAPABILITY_SCHEMA,
        "backend": "carla",
        "available": available,
        "connected": False,
        "clock": {"domain": "simulation", "manual_tick": True},
        "control": {
            "command": "throttle/brake/steer/gear",
            "normalized_ranges": {
                "throttle": [0.0, 1.0],
                "brake": [0.0, 1.0],
                "steer": [-1.0, 1.0],
            },
        },
        "sensors": [
            asdict(SensorSpec("camera", "rgb", "camera", required=False)),
            asdict(SensorSpec("lidar", "ray_cast", "lidar", required=False)),
            asdict(SensorSpec("imu", "imu", "vehicle", required=False)),
            asdict(SensorSpec("gnss", "gnss", "world", required=False)),
        ],
        "limitations": [
            "The bridge does not implement planning or safety policy.",
            "A CARLA server and compatible Python API are required for connect.",
        ],
        "error": reason,
    }


def _flowsim_spec() -> dict[str, Any]:
    launcher = ROOT / "build" / "bin" / "flow_launcher"
    pipeline = ROOT / "config" / "pipeline.json"
    return {
        "schema": CAPABILITY_SCHEMA,
        "backend": "flowsim",
        "available": launcher.is_file() and pipeline.is_file(),
        "connected": False,
        "clock": {"domain": "simulation", "manual_tick": True},
        "control": {
            "command": "control/cmd",
            "normalized_ranges": {
                "throttle": [0.0, 1.0],
                "brake": [0.0, 1.0],
                "steer": [-1.0, 1.0],
            },
        },
        "sensors": [
            asdict(SensorSpec("lidar", "point_cloud", "ego", required=False)),
            asdict(SensorSpec("gps", "position", "world", required=False)),
        ],
        "limitations": [
            "FlowSim is exercised through flow_launcher, not this CARLA adapter.",
        ],
        "error": None if launcher.is_file() and pipeline.is_file()
        else "FlowSim build or pipeline is unavailable",
    }


def capability_manifest() -> dict[str, Any]:
    """Return a truthful manifest for both the built-in and optional backends."""
    return {
        "schema": CAPABILITY_SCHEMA,
        "backends": [_flowsim_spec(), _carla_spec()],
    }


def _require_carla() -> Any:
    try:
        return importlib.import_module("carla")
    except (ImportError, OSError) as exc:
        raise CarlaUnavailableError(
            "CARLA Python API is not installed; install a compatible carla package "
            "and start a CARLA server before using --connect"
        ) from exc


def _snapshot_clock(snapshot: Any) -> SimulationClock:
    timestamp = getattr(snapshot, "timestamp", None)
    if timestamp is None:
        raise ValueError("CARLA snapshot has no timestamp")
    frame = int(getattr(snapshot, "frame"))
    sim_time_s = float(getattr(timestamp, "elapsed_seconds"))
    delta_s = float(getattr(timestamp, "delta_seconds"))
    if not math.isfinite(sim_time_s) or not math.isfinite(delta_s) or delta_s < 0.0:
        raise ValueError("CARLA snapshot timestamp is invalid")
    return SimulationClock(frame, sim_time_s, delta_s)


def to_carla_control(command: ControlCommand, carla_module: Any) -> Any:
    """Map the neutral command into CARLA without adding driving policy."""
    normalized = normalize_control(command)
    return carla_module.VehicleControl(
        throttle=normalized.throttle,
        brake=normalized.brake,
        steer=normalized.steer,
        gear=normalized.gear,
        reverse=normalized.gear < 0,
        hand_brake=normalized.hand_brake,
    )


class CarlaBridge:
    """Thin simulator-I/O adapter; no planner or evaluator logic lives here."""

    def __init__(self, host: str = "127.0.0.1", port: int = 2000,
                 timeout_s: float = 5.0, queue_depth: int = 8):
        if not host:
            raise ValueError("host must not be empty")
        if not 1 <= port <= 65535:
            raise ValueError("port must be between 1 and 65535")
        if timeout_s <= 0.0:
            raise ValueError("timeout_s must be positive")
        if queue_depth <= 0:
            raise ValueError("queue_depth must be positive")
        self.host = host
        self.port = port
        self.timeout_s = timeout_s
        self._carla: Any | None = None
        self._client: Any | None = None
        self._world: Any | None = None
        self._ego: Any | None = None
        self._sensor_specs: dict[str, SensorSpec] = {}
        self._sensor_queues: dict[str, Deque[SensorPacket]] = {}
        self._queue_depth = queue_depth

    @property
    def connected(self) -> bool:
        return self._world is not None

    def connect(self) -> None:
        if self.connected:
            return
        self._carla = _require_carla()
        client = self._carla.Client(self.host, self.port)
        client.set_timeout(self.timeout_s)
        self._client = client
        self._world = client.get_world()

    def bind_ego(self, actor: Any) -> None:
        if not self.connected:
            raise RuntimeError("connect the CARLA bridge before binding an ego actor")
        if not hasattr(actor, "apply_control"):
            raise TypeError("ego actor must expose apply_control")
        self._ego = actor

    def apply_control(self, command: ControlCommand) -> None:
        if self._ego is None:
            raise RuntimeError("no ego actor is bound")
        self._ego.apply_control(to_carla_control(command, self._carla))

    def tick(self) -> SimulationClock:
        if self._world is None:
            raise RuntimeError("CARLA bridge is not connected")
        self._world.tick()
        return _snapshot_clock(self._world.get_snapshot())

    def register_sensor(self, name: str, actor: Any, spec: SensorSpec) -> None:
        if not name or name != spec.name:
            raise ValueError("sensor name must match SensorSpec.name")
        if not hasattr(actor, "listen"):
            raise TypeError("sensor actor must expose listen")
        if name in self._sensor_specs:
            raise ValueError(f"sensor already registered: {name}")
        queue: Deque[SensorPacket] = deque(maxlen=self._queue_depth)
        self._sensor_specs[name] = spec
        self._sensor_queues[name] = queue

        def on_data(data: Any) -> None:
            frame = int(getattr(data, "frame"))
            timestamp_s = float(getattr(data, "timestamp"))
            if not math.isfinite(timestamp_s):
                raise ValueError(f"sensor {name} emitted a non-finite timestamp")
            queue.append(SensorPacket(name, frame, timestamp_s, data))

        actor.listen(on_data)

    def read_sensor_batch(self, clock: SimulationClock) -> SensorBatch:
        packets: dict[str, SensorPacket] = {}
        for name, queue in self._sensor_queues.items():
            if queue:
                packet = queue[-1]
                if packet.frame <= clock.frame:
                    packets[name] = packet
        missing = [
            spec.name for name, spec in self._sensor_specs.items()
            if spec.required and name not in packets
        ]
        if missing:
            raise RuntimeError(
                f"required CARLA sensors missing for frame {clock.frame}: {', '.join(missing)}"
            )
        return SensorBatch(clock=clock, packets=packets)

    def close(self) -> None:
        self._ego = None
        self._sensor_specs.clear()
        self._sensor_queues.clear()
        self._world = None
        self._client = None
        self._carla = None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    capabilities = sub.add_parser("capabilities", help="print backend capability manifests")
    capabilities.add_argument("--json", action="store_true", dest="as_json")
    check = sub.add_parser("check", help="fail unless the CARLA Python API is installed")
    check.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)

    if args.command == "capabilities":
        manifest = capability_manifest()
        if args.as_json:
            print(json.dumps(manifest, indent=2, ensure_ascii=False))
        else:
            for backend in manifest["backends"]:
                status = "available" if backend["available"] else "unavailable"
                detail = backend["error"] or ""
                print(f"{backend['backend']}: {status} {detail}".rstrip())
        return 0

    carla = _carla_spec()
    if args.as_json:
        print(json.dumps(carla, indent=2, ensure_ascii=False))
    else:
        print(carla["error"] or "CARLA Python API is available")
    return 0 if carla["available"] else 2


if __name__ == "__main__":
    sys.exit(main())
