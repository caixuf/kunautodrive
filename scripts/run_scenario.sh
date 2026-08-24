#!/bin/bash
# =============================================================================
# scripts/run_scenario.sh — 统一场景切换执行器 (被 monitor_server & Web 调度)
# =============================================================================
set -e

SCENARIO="${1:-scenarios/straight_road.json}"
ROUTE_ID="${2:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# 1. 杀死旧的 flow_launcher
pkill -9 -x flow_launcher 2>/dev/null || true
sleep 0.2

# 2. Patch pipeline.json
PIPELINE_ORIG="$ROOT/config/pipeline.json"
PIPELINE_TMP="/tmp/flow_pipeline_active.json"
SCENARIO_ABS="$([ -f "$SCENARIO" ] && echo "$(cd "$(dirname "$SCENARIO")" && pwd)/$(basename "$SCENARIO")" || echo "$SCENARIO")"

if [ -n "$ROUTE_ID" ]; then
  ROUTE_TMP="$(dirname "$SCENARIO_ABS")/.route_$(basename "$SCENARIO_ABS")"
  if command -v jq >/dev/null 2>&1; then
    jq --arg route_id "$ROUTE_ID" '.route_id = $route_id' "$SCENARIO_ABS" > "$ROUTE_TMP" 2>/dev/null || cp "$SCENARIO_ABS" "$ROUTE_TMP"
  else
    cp "$SCENARIO_ABS" "$ROUTE_TMP"
  fi
  SCENARIO_ABS="$ROUTE_TMP"
fi

python3 - "$PIPELINE_ORIG" "$PIPELINE_TMP" "$SCENARIO_ABS" <<'PY'
import json, sys
src, dst, scenario = sys.argv[1:4]
with open(src, "r", encoding="utf-8") as f:
    pipeline = json.load(f)
for process in pipeline.get("processes", []):
    if process.get("name") == "flowsim":
        raw = process.get("params", "{}")
        params = json.loads(raw) if isinstance(raw, str) else (raw or {})
        params["scenario_file"] = scenario
        process["params"] = json.dumps(params, ensure_ascii=False)
with open(dst, "w", encoding="utf-8") as f:
    json.dump(pipeline, f, indent=2, ensure_ascii=False)
PY

# 3. 启动新 flow_launcher
"$ROOT/build/bin/flow_launcher" "$PIPELINE_TMP" > /tmp/flow_launcher_stdout.txt 2> /tmp/flow_launcher_stderr.txt &
NEW_PID=$!
echo "Started flow_launcher PID $NEW_PID for scenario $SCENARIO_ABS"
