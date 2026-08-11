#!/usr/bin/env python3
"""Zero-dependency contracts and reference adapters for VLA/world-model work.

This module is deliberately an adapter boundary, not a claim that a VLA or a
generative world model is already present. Real models can implement the two
protocols and keep the existing FlowEngine topics/evaluators unchanged.
"""

from __future__ import annotations

import argparse
import json
import math
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Protocol

SCHEMA_VERSION = "flowengine.ai_contracts.v1"


@dataclass
class VLAInput:
    timestamp_s: float
    instruction: str
    ego: dict
    obstacles: list[dict] = field(default_factory=list)
    route: list[dict] = field(default_factory=list)
    scene_context: dict = field(default_factory=dict)
    history: list[dict] = field(default_factory=list)


@dataclass
class VLAOutput:
    target_speed_mps: float
    lateral_offset_m: float
    maneuver: str
    confidence: float
    rationale: str = ""


class VLAAdapter(Protocol):
    def predict(self, request: VLAInput) -> VLAOutput:
        """Map multimodal scene context and instruction to a planning intent."""


@dataclass
class WorldModelState:
    timestamp_s: float
    ego: dict
    obstacles: list[dict] = field(default_factory=list)
    scene_context: dict = field(default_factory=dict)


@dataclass
class WorldModelAction:
    throttle: float = 0.0
    brake: float = 0.0
    steer: float = 0.0


@dataclass
class WorldModelPrediction:
    next_state: WorldModelState
    events: list[str] = field(default_factory=list)
    uncertainty: dict = field(default_factory=dict)


class WorldModelAdapter(Protocol):
    def predict(self, state: WorldModelState, action: WorldModelAction,
                dt_s: float) -> WorldModelPrediction:
        """Predict the next state and uncertainty without mutating input state."""


class ReferenceVLA:
    """Deterministic planning-intent adapter used for contract and smoke tests.

    It is intentionally not a learned VLA. It provides a safe, repeatable
    target for wiring, replay, and evaluator integration before a real model is
    available.
    """

    def predict(self, request: VLAInput) -> VLAOutput:
        ego_speed = float(request.ego.get("speed", request.ego.get("v", 0.0)) or 0.0)
        speed_limit = float(request.scene_context.get("speed_limit", 22.0) or 22.0)
        target = speed_limit
        maneuver = "cruise"
        rationale = "reference speed-limit policy"

        lights = request.scene_context.get("traffic_light", {})
        if isinstance(lights, dict) and lights.get("state") == "red":
            target = 0.0
            maneuver = "stop"
            rationale = "reference red-light stop"

        ahead = [
            obs for obs in request.obstacles
            if float(obs.get("x", 1e9) or 1e9) > float(request.ego.get("x", 0.0) or 0.0)
        ]
        if ahead:
            lead = min(ahead, key=lambda obs: float(obs.get("x", 1e9) or 1e9))
            gap = float(lead.get("x", 1e9) or 1e9) - float(request.ego.get("x", 0.0) or 0.0)
            lead_speed = float(lead.get("vx", lead.get("speed", target)) or 0.0)
            if gap < max(8.0, ego_speed * 1.5):
                target = min(target, max(0.0, lead_speed))
                maneuver = "follow" if target > 0.1 else "stop"
                rationale = "reference time-headway policy"

        return VLAOutput(
            target_speed_mps=max(0.0, min(target, speed_limit)),
            lateral_offset_m=0.0,
            maneuver=maneuver,
            confidence=0.25,
            rationale=rationale,
        )


class KinematicWorldModel:
    """Small deterministic rollout model for adapter and scenario smoke tests."""

    def __init__(self, wheelbase_m: float = 2.7, max_accel_mps2: float = 3.0,
                 max_brake_mps2: float = 6.0):
        if wheelbase_m <= 0.0 or max_accel_mps2 <= 0.0 or max_brake_mps2 <= 0.0:
            raise ValueError("world-model limits must be positive")
        self.wheelbase_m = wheelbase_m
        self.max_accel_mps2 = max_accel_mps2
        self.max_brake_mps2 = max_brake_mps2

    def predict(self, state: WorldModelState, action: WorldModelAction,
                dt_s: float) -> WorldModelPrediction:
        if dt_s <= 0.0:
            raise ValueError("dt_s must be positive")
        ego = dict(state.ego)
        x = float(ego.get("x", 0.0) or 0.0)
        y = float(ego.get("y", 0.0) or 0.0)
        speed = max(0.0, float(ego.get("speed", ego.get("v", 0.0)) or 0.0))
        heading = float(ego.get("heading", 0.0) or 0.0)
        throttle = max(0.0, min(1.0, float(action.throttle)))
        brake = max(0.0, min(1.0, float(action.brake)))
        steer = max(-1.0, min(1.0, float(action.steer)))
        acceleration = throttle * self.max_accel_mps2 - brake * self.max_brake_mps2
        next_speed = max(0.0, speed + acceleration * dt_s)
        yaw_rate = next_speed / self.wheelbase_m * math.tan(steer * 0.6)
        next_heading = heading + yaw_rate * dt_s
        next_x = x + next_speed * math.cos(next_heading) * dt_s
        next_y = y + next_speed * math.sin(next_heading) * dt_s
        ego.update({"x": next_x, "y": next_y, "speed": next_speed,
                    "heading": next_heading, "yaw_rate": yaw_rate})

        events: list[str] = []
        for obstacle in state.obstacles:
            ox = float(obstacle.get("x", 1e9) or 1e9)
            oy = float(obstacle.get("y", 1e9) or 1e9)
            if math.hypot(ox - next_x, oy - next_y) < 2.0:
                events.append("collision_risk")
                break
        return WorldModelPrediction(
            next_state=WorldModelState(
                timestamp_s=state.timestamp_s + dt_s,
                ego=ego,
                obstacles=[dict(obs) for obs in state.obstacles],
                scene_context=dict(state.scene_context),
            ),
            events=events,
            uncertainty={"position_std_m": 0.05, "speed_std_mps": 0.1},
        )


def rollout_reference(scenario: dict, steps: int, dt_s: float,
                      instruction: str) -> dict:
    ego = dict(scenario.get("ego", {}))
    obstacles = [dict(actor) for actor in scenario.get("actors", [])
                 if isinstance(actor, dict) and actor.get("type") != "ego"]
    edge = (scenario.get("road_network", {}).get("edges") or [{}])[0]
    context = {"speed_limit": float(edge.get("speed_limit", 22.0) or 22.0),
               "lane_width": float(edge.get("lane_width", 3.5) or 3.5)}
    vla = ReferenceVLA()
    world = KinematicWorldModel(float(ego.get("wheelbase", 2.7) or 2.7))
    state = WorldModelState(0.0, ego, obstacles, context)
    frames = []
    for _ in range(max(0, steps)):
        intent = vla.predict(VLAInput(state.timestamp_s, instruction, state.ego,
                                      state.obstacles, [], state.scene_context))
        current = float(state.ego.get("speed", 0.0) or 0.0)
        action = WorldModelAction(
            throttle=max(0.0, min(1.0, (intent.target_speed_mps - current) * 0.08)),
            brake=1.0 if intent.target_speed_mps <= 0.1 and current > 0.1 else 0.0,
        )
        prediction = world.predict(state, action, dt_s)
        frames.append({"t": prediction.next_state.timestamp_s,
                       "ego": prediction.next_state.ego,
                       "intent": asdict(intent),
                       "events": prediction.events})
        state = prediction.next_state
    return {"schema_version": SCHEMA_VERSION, "backend": "reference",
            "generated_at_unix_s": time.time(), "instruction": instruction,
            "frames": frames}


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the reference VLA/world-model contract.")
    parser.add_argument("--scenario", type=Path, required=True)
    parser.add_argument("--steps", type=int, default=20)
    parser.add_argument("--dt", type=float, default=0.1)
    parser.add_argument("--instruction", default="Drive safely and follow the route.")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    scenario = json.loads(args.scenario.read_text(encoding="utf-8"))
    result = rollout_reference(scenario, args.steps, args.dt, args.instruction)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n",
                           encoding="utf-8")
    print(f"wrote {args.output} ({len(result['frames'])} frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
