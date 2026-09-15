#!/bin/bash
# 验证 production monitor + FlowCoro PEM 业务采集器的真实写入链路。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DURATION="${1:-4}"
WORK="$ROOT/build/pem_smoke_$$"
PIPELINE="$WORK/pipeline.json"
LOG="$WORK/launcher.log"
KEEP_LOG=0
mkdir -p "$WORK"

cleanup() {
    if [ "$KEEP_LOG" != 0 ]; then
        echo "───── launcher log tail ─────"
        if [ -f "$LOG" ]; then
            tail -80 "$LOG"
        else
            echo "(no launcher.log)"
        fi
        echo "───── flow logs tail ─────"
        if [ -d "$WORK/logs" ]; then
            for f in "$WORK"/logs/*.log; do
                [ -f "$f" ] || continue
                echo "--- $(basename "$f") ---"
                tail -20 "$f"
            done
        fi
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ ! -x "$BUILD_DIR/bin/flow_launcher" ] ||
   [ ! -f "$BUILD_DIR/lib/libpem_collector_node.so" ]; then
    echo "FAIL: flow_launcher or pem_collector_node is not built"
    exit 1
fi

echo "INFO: writing production PEM pipeline under $WORK"
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
            "state_file": os.path.join(os.path.dirname(os.environ["PEM_BASE"]),
                                       "topology.json"),
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
export FLOW_LOG_DIR="$WORK/logs"
mkdir -p "$FLOW_LOG_DIR"

# SIGTERM at 60s, SIGKILL 5s later. Unbounded `timeout 90` waited forever when
# flow_launcher ignored SIGTERM inside RtExecutor::shutdown(), so ctest's 150s
# property fired with empty output. Duration is 4s plus ~5s node stagger.
echo "INFO: running flow_launcher (duration=${DURATION}s, hard cap 60s)"
set +e
timeout --kill-after=5 60 "$BUILD_DIR/bin/flow_launcher" "$PIPELINE" --duration "$DURATION" >"$LOG" 2>&1
launcher_rc=$?
set -e

if [ "$launcher_rc" -ne 0 ]; then
    KEEP_LOG=1
    if [ "$launcher_rc" -eq 124 ] || [ "$launcher_rc" -eq 137 ]; then
        echo "FAIL: flow_launcher timed out (rc=$launcher_rc)"
    else
        echo "FAIL: flow_launcher exited with code $launcher_rc"
    fi
    exit 1
fi

shopt -s nullglob
files=("$WORK"/pem_business_*.pem)
if [ "${#files[@]}" -eq 0 ]; then
    KEEP_LOG=1
    echo "FAIL: PEM business stream was not created"
    exit 1
fi

decoded="$(python3 tools/pem_dump.py --jsonl --type business "${files[@]}")"
if ! grep -q '"name": "trip:ci_simulation"' <<<"$decoded"; then
    KEEP_LOG=1
    echo "FAIL: PEM business stream has no trip record"
    printf '%s\n' "$decoded"
    exit 1
fi
echo "PASS: PEM runtime smoke"
