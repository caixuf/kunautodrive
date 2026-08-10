#!/usr/bin/env bash
set -euo pipefail

if [ -z "${PRODUCT_HOME:-}" ]; then
    case "$0" in
        */run) PRODUCT_HOME="$(cd "$(dirname "$0")" && pwd)" ;;
        *)
            echo "PRODUCT_HOME is required when product_run.sh is called directly" >&2
            exit 1
            ;;
    esac
fi
RELEASE="$PRODUCT_HOME/current"
PRODUCT_CONFIG="$RELEASE/product.json"
PRODUCT_ID="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["product_id"])' "$PRODUCT_CONFIG")"
PLUGIN_DIR="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["plugin_dir"])' "$PRODUCT_CONFIG")"
SHARE="$RELEASE/share/$PRODUCT_ID"
DEFAULT_VEHICLE="$(python3 "$SHARE/tools/product_config.py" \
    "$PRODUCT_CONFIG" default_vehicle)"
PROFILE="${FLOWENGINE_VEHICLE:-${1:-$DEFAULT_VEHICLE}}"
PROFILE_FILE="$PRODUCT_HOME/etc/vehicles/${PROFILE}.json"
PIPELINE="$PRODUCT_HOME/var/run/pipeline.json"

if [ ! -x "$RELEASE/bin/flow_launcher" ]; then
    echo "Missing active release: $RELEASE/bin/flow_launcher" >&2
    exit 1
fi
if [ ! -f "$PROFILE_FILE" ]; then
    echo "Unknown vehicle profile: $PROFILE_FILE" >&2
    exit 1
fi

mkdir -p "$PRODUCT_HOME/var/run" "$PRODUCT_HOME/var/log" \
         "$PRODUCT_HOME/var/data" "$PRODUCT_HOME/var/models"

export FLOWENGINE_HOME="$RELEASE"
export PRODUCT_HOME
export FLOWENGINE_TEMP_DIR="$PRODUCT_HOME/var/run"
export PATH="$RELEASE/bin:$PATH"
clean_ld_path=""
IFS=: read -ra ld_entries <<< "${LD_LIBRARY_PATH:-}"
for entry in "${ld_entries[@]}"; do
    case "$entry" in
        *"/remote-agent"*) ;;
        "") ;;
        *) clean_ld_path="${clean_ld_path:+$clean_ld_path:}$entry" ;;
    esac
done
export LD_LIBRARY_PATH="$RELEASE/lib:$RELEASE/$PLUGIN_DIR${clean_ld_path:+:$clean_ld_path}"

BASE_PIPELINE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["base_pipeline"])' \
    "$PROFILE_FILE")"
python3 "$SHARE/tools/vehicle_config.py" \
    --base "$SHARE/config/$BASE_PIPELINE" \
    --profile "$PROFILE_FILE" \
    --plugin-dir "$RELEASE/$PLUGIN_DIR" --plugin-rel-dir "$PLUGIN_DIR" \
    --output "$PIPELINE" --check

cd "$RELEASE"
exec "$RELEASE/bin/flow_launcher" "$PIPELINE"
