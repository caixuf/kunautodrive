#!/bin/bash
set -e
BUILD_TYPE=Release
STRIP=false
PREFIX=""
PACKAGE_MODE=false
ARCH="$(uname -m)"

for arg in "$@"; do
    case "$arg" in
        --release) BUILD_TYPE=Release ;;
        --debug)   BUILD_TYPE=Debug ;;
        --strip)   STRIP=true ;;
        --package) PACKAGE_MODE=true ;;
        --*)       echo "Unknown option: $arg"; exit 1 ;;
        *)         PREFIX="$arg" ;;
    esac
done

if [ "$PACKAGE_MODE" = false ] && [ -z "$PREFIX" ]; then
    echo "Usage: bash scripts/deploy.sh [--release|--debug] [--strip] <target_dir>"
    echo "   or: bash scripts/deploy.sh --package [--release|--debug|--strip]"
    exit 1
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PRODUCT_CONFIG="$ROOT/config/product.json"
PACKAGE_PREFIX="$(python3 "$ROOT/tools/product_config.py" "$PRODUCT_CONFIG" package_prefix)"
PRODUCT_ID="$(python3 "$ROOT/tools/product_config.py" "$PRODUCT_CONFIG" product_id)"
PLUGIN_DIR="$(python3 "$ROOT/tools/product_config.py" "$PRODUCT_CONFIG" plugin_dir)"
BUILD_DIR="$ROOT/build_deploy"
NODES_BUILD_DIR="$BUILD_DIR/modules/adas_nodes"

if [ "$PACKAGE_MODE" = true ]; then
    VERSION=$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo "dev")
    case "$ARCH" in
        x86_64|amd64) ARCH=x86_64 ;;
        aarch64|arm64) ARCH=arm64 ;;
    esac
    RELEASE_NAME="${PACKAGE_PREFIX}-${VERSION}-linux-${ARCH}"
    TARNAME="${RELEASE_NAME}.tar.gz"
    DIST_DIR="$ROOT/dist"
    mkdir -p "$DIST_DIR"
    STAGE_DIR="$(mktemp -d /tmp/flowengine_pkg_XXXXXX)"
    trap 'rm -rf "$STAGE_DIR"' EXIT
    PREFIX="$STAGE_DIR/$RELEASE_NAME"
fi

echo "[1/4] Building..."
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_INSTALL_PREFIX="$PREFIX" > /dev/null 2>&1
cmake --build "$BUILD_DIR" -j$(nproc) 2>/dev/null
cmake -S "$ROOT/modules/adas_nodes" -B "$NODES_BUILD_DIR" \
    -DFLOWENGINE_BUILD="$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" > /dev/null 2>&1
cmake --build "$NODES_BUILD_DIR" -j$(nproc) 2>/dev/null
echo "[2/4] Installing..."
cmake --install "$BUILD_DIR" 2>/dev/null
cmake --install "$NODES_BUILD_DIR" 2>/dev/null
if $STRIP; then echo "[3/4] Stripping..."; find "$PREFIX/bin" "$PREFIX/lib" -type f -executable -o -name "*.so" | xargs strip 2>/dev/null || true; else echo "[3/4] Skipping strip"; fi
echo "[4/4] Environment..."
cat > "$PREFIX/flowengine.env" << EOF
export FLOWENGINE_HOME="$PREFIX"
export PATH="\$FLOWENGINE_HOME/bin:\$PATH"
export LD_LIBRARY_PATH="\$FLOWENGINE_HOME/lib:\$FLOWENGINE_HOME/$PLUGIN_DIR:\${LD_LIBRARY_PATH:-}"
EOF

if [ "$PACKAGE_MODE" = true ]; then
    mkdir -p "$PREFIX/share/$PRODUCT_ID/deploy"
    cp "$ROOT/scripts/product_install.sh" "$PREFIX/share/$PRODUCT_ID/deploy/"
    python3 - "$PREFIX" "$VERSION" "$ARCH" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
files = {}
for path in sorted(root.rglob("*")):
    if path.is_file():
        files[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
manifest = {
    "format_version": 1,
    "version": sys.argv[2],
    "platform": "linux",
    "architecture": sys.argv[3],
    "files": files,
}
(root / "manifest.json").write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
    tar czf "$DIST_DIR/$TARNAME" -C "$STAGE_DIR" "$RELEASE_NAME"
    cp "$ROOT/scripts/product_install.sh" "$DIST_DIR/${TARNAME%.tar.gz}.install.sh"
    chmod +x "$DIST_DIR/${TARNAME%.tar.gz}.install.sh"
    echo "✓ $DIST_DIR/$TARNAME"
    echo "✓ $DIST_DIR/${TARNAME%.tar.gz}.install.sh"
else
    echo "✓ Deploy to $PREFIX"
fi
