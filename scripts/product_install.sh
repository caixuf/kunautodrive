#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 <release.tar.gz> [product_dir]" >&2
    exit 1
fi

PACKAGE="$(readlink -f "$1")"
TOP="$(python3 - "$PACKAGE" <<'PY'
import sys
import tarfile
with tarfile.open(sys.argv[1], "r:gz") as archive:
    first = archive.next()
    if first is None:
        raise SystemExit("empty package")
    print(first.name.split("/", 1)[0])
PY
)"
PACKAGE_CONFIG="$(tar xOf "$PACKAGE" "$TOP/product.json")"
INSTALL_DIR="$(printf '%s' "$PACKAGE_CONFIG" | python3 -c 'import json,sys; print(json.load(sys.stdin)["install_dir"])')"
PRODUCT_HOME="${2:-$PWD/$INSTALL_DIR}"
mkdir -p "$PRODUCT_HOME/releases" "$PRODUCT_HOME/etc/vehicles" \
         "$PRODUCT_HOME/var/log" "$PRODUCT_HOME/var/data" \
         "$PRODUCT_HOME/var/models" "$PRODUCT_HOME/var/run"

PACKAGE_PREFIX="$(printf '%s' "$PACKAGE_CONFIG" | python3 -c 'import json,sys; print(json.load(sys.stdin)["package_prefix"])')"
case "$TOP" in
    "$PACKAGE_PREFIX"-*) ;;
    *) echo "Invalid package root: $TOP" >&2; exit 1 ;;
esac

RELEASE="$PRODUCT_HOME/releases/$TOP"
if [ -e "$RELEASE" ]; then
    echo "Release already exists: $RELEASE" >&2
    exit 1
fi
cleanup_failed_release() {
    if [ -d "$RELEASE" ] &&
       [ "$(dirname "$RELEASE")" = "$PRODUCT_HOME/releases" ] &&
       [ "$(basename "$RELEASE")" = "$TOP" ]; then
        rm -rf "$RELEASE"
    fi
}
trap cleanup_failed_release EXIT
tar xzf "$PACKAGE" -C "$PRODUCT_HOME/releases"
test -x "$RELEASE/bin/flow_launcher"
PRODUCT_ID="$(printf '%s' "$PACKAGE_CONFIG" | python3 -c 'import json,sys; print(json.load(sys.stdin)["product_id"])')"
SHARE="$RELEASE/share/$PRODUCT_ID"
test -f "$SHARE/config/pipeline_car.json"
python3 "$SHARE/tools/verify_release.py" "$RELEASE"

for profile in "$SHARE/vehicles/"*.json; do
    name="$(basename "$profile")"
    cp "$profile" "$PRODUCT_HOME/etc/vehicles/${name%.json}.dist.json"
    if [ ! -e "$PRODUCT_HOME/etc/vehicles/$name" ]; then
        cp "$profile" "$PRODUCT_HOME/etc/vehicles/$name"
    elif ! cmp -s "$profile" "$PRODUCT_HOME/etc/vehicles/$name"; then
        echo "Profile update available: etc/vehicles/${name%.json}.dist.json"
    fi
done
ln -sfn "releases/$TOP" "$PRODUCT_HOME/current.new"
mv -Tf "$PRODUCT_HOME/current.new" "$PRODUCT_HOME/current"
ln -sfn "current/share/$PRODUCT_ID/scripts/product_run.sh" \
        "$PRODUCT_HOME/run"
trap - EXIT

echo "Installed $TOP to $PRODUCT_HOME"
echo "Run: $PRODUCT_HOME/run"
