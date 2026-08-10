#!/bin/bash
# 验证 production monitor + FlowCoro PEM 业务采集器的真实写入链路。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DURATION="${1:-4}"
WORK="$(mktemp -d /tmp/kunautodrive_pem_smoke.XXXXXX)"
PIPELINE="$WORK/pipeline.json"
LOG="$WORK/launcher.log"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$BUILD_DIR/bin/flow_launcher" ] ||
   [ ! -f "$BUILD_DIR/lib/libpem_collector_node.so" ]; then
    echo "FAIL: flow_launcher or pem_collector_node is not built"
    exit 1
fi

ROOT="$ROOT" PIPELINE="$PIPELINE" PEM_BASE="$WORK/pem" python3 - <<'PY'
import json
import os

root = os.environ["ROOT"]
with open(os.path.join(root, "config", "pipeline.json"), encoding="utf-8") as f:
    pipeline = json.load(f)
for process in pipeline["processes"]:
    if process["name"] == "monitor":
        process["params"] = json.dumps({
            "mode": "production", "frequency_hz": 2,
            "pem_log_path": os.environ["PEM_BASE"] + "_infra",
            "rotate_sec": 300, "rotate_mb": 1,
            "retain_segments": 2, "retain_mb": 2,
        }, separators=(",", ":"))
        process["publish"] = [{"topic": "pem/degrade_event", "type": "text"}]
pipeline["processes"].append({
    "name": "pem_collector",
    "library_path": "build/lib/libpem_collector_node.so",
    "auto_start": True,
    "subscribe": ["sensor/gps", "fusion/localization", "pem/degrade_event"],
    "params": json.dumps({
        "emit_hz": 2, "region": "ci_simulation",
        "pem_log_path": os.environ["PEM_BASE"] + "_business",
        "rotate_sec": 300, "rotate_mb": 1,
        "retain_segments": 2, "retain_mb": 2,
    }, separators=(",", ":")),
})
with open(os.environ["PIPELINE"], "w", encoding="utf-8") as f:
    json.dump(pipeline, f)
PY

cd "$ROOT"
timeout 90 "$BUILD_DIR/bin/flow_launcher" "$PIPELINE" --duration "$DURATION" >"$LOG" 2>&1

shopt -s nullglob
files=("$WORK"/pem_business_*.pem)
if [ "${#files[@]}" -eq 0 ]; then
    echo "FAIL: PEM business stream was not created"
    tail -40 "$LOG"
    exit 1
fi

decoded="$(python3 tools/pem_dump.py --jsonl --type business "${files[@]}")"
if ! grep -q '"name": "trip:ci_simulation"' <<<"$decoded"; then
    echo "FAIL: PEM business stream has no trip record"
    printf '%s\n' "$decoded"
    exit 1
fi
echo "PASS: PEM runtime smoke"
