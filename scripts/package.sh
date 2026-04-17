#!/bin/bash
# package.sh — Build and package all bpt-core services for deployment.
#
# Usage:
#   ./scripts/package.sh [--version VERSION] [--build-dir DIR] [--out-dir DIR]
#
# Outputs:
#   bpt-core-{version}-linux-x86_64.tar.gz in --out-dir (default: .)
#
# On the target host, extract and run install.sh to configure the system.

set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build-release"
OUT_DIR="$REPO_DIR"
VERSION=""

# ── Parse args ────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version)   VERSION="$2";   shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --out-dir)   OUT_DIR="$2";   shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Default version: git tag if on a tag, else YYYY-MM-DD-<short-sha>
if [ -z "$VERSION" ]; then
    GIT_TAG=$(git -C "$REPO_DIR" describe --exact-match --tags 2>/dev/null || true)
    if [ -n "$GIT_TAG" ]; then
        VERSION="${GIT_TAG#v}"
    else
        SHA=$(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null || echo "unknown")
        VERSION="$(date -u +%Y-%m-%d)-$SHA"
    fi
fi

PACKAGE_NAME="bpt-core-${VERSION}-linux-x86_64"
STAGE_DIR="$(mktemp -d)"
PKG_DIR="$STAGE_DIR/$PACKAGE_NAME"

echo "=== bpt-core package build ==="
echo "  Version   : $VERSION"
echo "  Build dir : $BUILD_DIR"
echo "  Output    : $OUT_DIR/$PACKAGE_NAME.tar.gz"
echo

cleanup() { rm -rf "$STAGE_DIR"; }
trap cleanup EXIT

# ── Step 1: C++ Release build ─────────────────────────────────────
echo "--- Building C++ services (Release) ---"

VCPKG_ROOT="${VCPKG_ROOT:-$REPO_DIR/vcpkg}"

CMAKE_ARGS=(
    -B "$BUILD_DIR"
    -G Ninja
    -DCMAKE_BUILD_TYPE=Release
)

if [ -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")
fi

cmake -S "$REPO_DIR" "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo

# ── Step 2: Bifrost-fabric shadowJar ──────────────────────────────
echo "--- Building transport shadowJar ---"
TRANSPORT_DIR="$REPO_DIR/transport"
"$TRANSPORT_DIR/gradlew" -p "$TRANSPORT_DIR" shadowJar -q
BIFROST_JAR=$(find "$TRANSPORT_DIR/build/libs" -name "*-all.jar" -type f | head -1)
echo "  JAR: $BIFROST_JAR"
echo

# ── Step 3: Assemble package directory ────────────────────────────
echo "--- Assembling package ---"
mkdir -p "$PKG_DIR"

# Helper: copy a C++ service
copy_service() {
    local svc="$1"
    local binary_name="${2:-$svc}"     # name of the CMake executable
    local install_name="${3:-$svc}"    # name in bin/ (may differ from cmake target)

    local svc_dir="$PKG_DIR/$svc"
    mkdir -p "$svc_dir/bin" "$svc_dir/logs"

    # Binary
    local src_bin="$BUILD_DIR/$svc/src/$binary_name"
    if [ ! -f "$src_bin" ]; then
        echo "ERROR: Binary not found: $src_bin"
        exit 1
    fi
    cp "$src_bin" "$svc_dir/bin/$install_name"
    chmod 755 "$svc_dir/bin/$install_name"

    # Config
    if [ -d "$REPO_DIR/$svc/config" ]; then
        cp -r "$REPO_DIR/$svc/config" "$svc_dir/"
    fi

    # Scripts
    if [ -d "$REPO_DIR/$svc/scripts" ]; then
        mkdir -p "$svc_dir/scripts"
        cp "$REPO_DIR/$svc/scripts"/*.sh "$svc_dir/scripts/" 2>/dev/null || true
        chmod 755 "$svc_dir/scripts"/*.sh 2>/dev/null || true
    fi

    echo "  $svc -> $svc_dir"
}

copy_service bpt-strategy
copy_service bpt-md-gateway
copy_service order-gateway
copy_service bpt-refdata
copy_service bpt-pricer bpt-pricer_app bpt-pricer
copy_service bpt-backtester

# Bifrost-fabric
BF_DIR="$PKG_DIR/transport"
mkdir -p "$BF_DIR/bin" "$BF_DIR/logs"
cp "$BIFROST_JAR" "$BF_DIR/bin/transport-all.jar"
if [ -d "$TRANSPORT_DIR/config" ]; then
    cp -r "$TRANSPORT_DIR/config" "$BF_DIR/"
fi
mkdir -p "$BF_DIR/scripts"
cp "$TRANSPORT_DIR/scripts"/*.sh "$BF_DIR/scripts/" 2>/dev/null || true
chmod 755 "$BF_DIR/scripts"/*.sh 2>/dev/null || true
echo "  transport -> $BF_DIR"

# Top-level orchestration scripts
mkdir -p "$PKG_DIR/scripts"
cp "$REPO_DIR/scripts/stack.sh" "$PKG_DIR/scripts/"
cp "$REPO_DIR/scripts/stack-testnet.sh" "$PKG_DIR/scripts/"
chmod 755 "$PKG_DIR/scripts"/*.sh
echo "  scripts/ -> $PKG_DIR/scripts/"

# install.sh — run once on the target host
cat > "$PKG_DIR/install.sh" <<'INSTALL_EOF'
#!/bin/bash
# install.sh — Install system dependencies on the target host.
# Run once as root (or with sudo) after extracting the package.
set -euo pipefail

echo "Installing bpt-core system dependencies..."

# Arrow & Parquet (Apache official apt repo — Ubuntu 24.04 noble)
wget -q https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-noble.deb
apt-get install -y ./apache-arrow-apt-source-latest-noble.deb
rm apache-arrow-apt-source-latest-noble.deb
apt-get update -q
apt-get install -y libarrow-dev libparquet-dev

# OpenSSL 3, libstdc++
apt-get install -y libssl3 libstdc++6

# Java 17 (for transport)
apt-get install -y openjdk-17-jre-headless

echo "Done. Extract the package and update configs, then run: ./scripts/stack.sh start"
INSTALL_EOF
chmod 755 "$PKG_DIR/install.sh"

echo

# ── Step 4: Create tarball ────────────────────────────────────────
echo "--- Creating tarball ---"
mkdir -p "$OUT_DIR"
TARBALL="$OUT_DIR/$PACKAGE_NAME.tar.gz"
tar -czf "$TARBALL" -C "$STAGE_DIR" "$PACKAGE_NAME"
echo
echo "=== Package ready: $TARBALL ==="
echo "  Size: $(du -sh "$TARBALL" | cut -f1)"
echo
echo "Deploy with:"
echo "  scp $TARBALL user@host:/opt/bpt/"
echo "  ssh user@host 'cd /opt/bpt && tar -xzf $PACKAGE_NAME.tar.gz && cd $PACKAGE_NAME && sudo ./install.sh'"
