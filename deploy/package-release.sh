#!/bin/bash
# package-release.sh — build + package a deployable release tarball.
#
# Produces: dist/bpt-<version>.tar.gz
#
# Layout of the produced tarball (matches /opt/bpt/releases/<ver>/ on target):
#
#   VERSION                       — text file, e.g. "v0.1.0+abc123"
#   bin/                          — all service executables (bazel-built) + Java jar
#     bpt-strategy
#     bpt-order-gateway
#     bpt-md-gateway
#     bpt-refdata
#     bpt-analytics
#     bpt-pricer
#     bpt-backtester
#     bridge
#     transport/bpt-transport-<version>-all.jar
#   share/
#     schema/bpt-protocol.xml     — SBE schema (for debugging / tool builds)
#     config-templates/           — per-strategy default TOMLs (reference only)
#     instruments/                — canonical instrument mapping JSONs
#   scripts/
#     generate-units.sh           — host-side: writes systemd units
#     sync-config.sh              — host-side: git pull for config refresh
#     cleanup-stale-logs.sh       — host-side: weekly log pruner
#     deploy.sh                   — host-side: unpack + symlink swap + restart
#     rollback.sh                 — host-side: flip current back to previous
#
# Usage:
#   ./deploy/package-release.sh          # auto-versions from git HEAD
#   VERSION=v0.2.0 ./deploy/package-release.sh   # override version label
#
# The tarball is self-contained and deterministic relative to the git SHA
# (plus whatever Bazel + Gradle build inputs produced). Re-running with the
# same SHA should produce a functionally identical artifact.

set -euo pipefail
# Empty globs collapse instead of iterating the literal pattern — otherwise
# `for f in */.json; do cp "$f" ...` fires set -e on the no-match iteration.
shopt -s nullglob

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

# ── Versioning ──────────────────────────────────────────────────────────────
# <semver>+<short-sha>[-dirty] — dirty suffix flags uncommitted changes so
# we can tell a built tarball apart from "last-pushed state" in incidents.
SEMVER="${SEMVER:-v0.1.0}"
SHA=$(git rev-parse --short=12 HEAD)
DIRTY=""
if ! git diff --quiet HEAD -- 2>/dev/null || ! git diff --cached --quiet 2>/dev/null; then
    DIRTY="-dirty"
fi
VERSION="${VERSION:-${SEMVER}+${SHA}${DIRTY}}"

echo "=== Packaging bpt release ${VERSION} ==="

# ── Build ───────────────────────────────────────────────────────────────────
echo "--- Building C++ services via Bazel..."
bazel build //...
echo "--- Building Java MediaDriver via Gradle..."
(cd transport/aeron && ./gradlew shadowJar --quiet)

# ── Stage ───────────────────────────────────────────────────────────────────
STAGE_ROOT=$(mktemp -d -t bpt-release-XXXXXX)
trap 'rm -rf "$STAGE_ROOT"' EXIT
# Stage under a version-named dir so tar produces a clean archive
# without needing --transform tricks.
STAGE="$STAGE_ROOT/bpt-${VERSION}"

mkdir -p "$STAGE"/{bin/transport,share/schema,share/config-templates,share/instruments,share/topology,scripts}
# Per-service config seeds. Mirrors the source-tree bpt-<svc>/config/ layout
# so deploy.sh can seed $BPT_DEPLOY_ROOT/<svc>/config/ on first install —
# that layout is what the generated systemd units expect at runtime
# (WorkingDirectory=$BPT_ROOT/$svc). See deploy/generate-units.sh.
mkdir -p "$STAGE"/share/service-configs/{refdata,md-gateway,order-gateway,strategy,analytics,bridge}

echo "--- Copying C++ binaries..."
# Map: source binary path → destination name under bin/
# NOTE: bpt-backtester is intentionally omitted — it's a CMake-built
# library+tests (arrow/parquet deps), not a runtime service. Backtests
# are run offline from the source checkout; no deployed backtester binary.
declare -A SERVICES=(
    ["bpt-strategy/bpt-strategy"]="bpt-strategy"
    ["bpt-order-gateway/bpt-order-gateway"]="bpt-order-gateway"
    ["bpt-md-gateway/bpt-md-gateway"]="bpt-md-gateway"
    ["bpt-refdata/bpt-refdata"]="bpt-refdata"
    ["bpt-analytics/bpt-analytics"]="bpt-analytics"
    ["bpt-pricer/bpt-pricer"]="bpt-pricer"
    ["dashboard/bridge/bridge"]="bridge"
)
for src in "${!SERVICES[@]}"; do
    dst="${SERVICES[$src]}"
    src_path="bazel-bin/$src"
    if [ -f "$src_path" ]; then
        cp "$src_path" "$STAGE/bin/$dst"
        chmod +x "$STAGE/bin/$dst"
    else
        echo "WARNING: missing binary $src_path (service not built?)" >&2
    fi
done

echo "--- Copying Java jar..."
cp transport/aeron/build/libs/bpt-transport-*-all.jar "$STAGE/bin/transport/"

echo "--- Copying schema + mappings + config templates..."
cp messages/schema/bpt-protocol.xml "$STAGE/share/schema/"
# Transport (Aeron MediaDriver) config — baked into release so the
# systemd unit on the host references a path inside the release root
# rather than needing a separate config layout for transport.
cp transport/aeron/config/config.yaml "$STAGE/share/transport.yaml"
# Canonical instrument mappings (consumed at runtime by bpt-refdata).
# Only ship runtime JSONs — skip Bazel BUILD files / anything else.
for f in config/instruments/*.json; do
    [ -f "$f" ] && cp "$f" "$STAGE/share/instruments/"
done
# Strategy default / shared parameter templates (reference material for operators).
for f in bpt-strategy/config/strategies/*.toml; do
    [ -f "$f" ] && cp "$f" "$STAGE/share/config-templates/"
done

echo "--- Copying per-service config seeds..."
# Per-service TOMLs + exchange-config subdirs. Layout inside each service
# seed mirrors exactly what the source-tree bpt-<svc>/config/ contains, so
# deploy.sh can cp -r share/service-configs/<svc>/. into $BPT_DEPLOY_ROOT/bpt-<svc>/config/.
# Exclude build artefacts (Bazel BUILD files, etc).
copy_service_configs() {
    local svc_src="$1" seed_dst="$2"
    for f in "$svc_src"/*.toml; do
        cp "$f" "$seed_dst/"
    done
    for d in "$svc_src"/*/; do
        local name
        name=$(basename "$d")
        case "$name" in
            BUILD|tests|__pycache__) continue ;;  # never ship build/test junk
        esac
        mkdir -p "$seed_dst/$name"
        for f in "$d"/*.toml "$d"/*.json; do
            cp "$f" "$seed_dst/$name/"
        done
    done
}
copy_service_configs bpt-refdata/config         "$STAGE/share/service-configs/refdata"
copy_service_configs bpt-md-gateway/config      "$STAGE/share/service-configs/md-gateway"
copy_service_configs bpt-order-gateway/config   "$STAGE/share/service-configs/order-gateway"
copy_service_configs bpt-strategy/config        "$STAGE/share/service-configs/strategy"
copy_service_configs bpt-analytics/config       "$STAGE/share/service-configs/analytics"
# Bridge lives under dashboard/bridge/config/
copy_service_configs dashboard/bridge/config    "$STAGE/share/service-configs/bridge"

echo "--- Copying CPU-affinity topology profiles..."
# deploy/topology/*.toml — per-host-class pinning profiles. Operator (or
# deploy.sh) picks one and points the active env at it.
for f in deploy/topology/*.toml; do
    [ -f "$f" ] && cp "$f" "$STAGE/share/topology/"
done

echo "--- Copying host-side scripts..."
cp deploy/generate-units.sh    "$STAGE/scripts/"
cp deploy/sync-config.sh       "$STAGE/scripts/" 2>/dev/null || true
cp scripts/cleanup-stale-logs.sh "$STAGE/scripts/" 2>/dev/null || true
# Deploy + rollback land here once they exist (TODO — follow-up commit).
cp deploy/deploy.sh            "$STAGE/scripts/" 2>/dev/null || true
cp deploy/rollback.sh          "$STAGE/scripts/" 2>/dev/null || true

echo "$VERSION" > "$STAGE/VERSION"
date -u +%FT%TZ > "$STAGE/BUILT_AT"

# ── Manifest (for introspection on target) ──────────────────────────────────
(
    echo "version: $VERSION"
    echo "built_at: $(cat "$STAGE/BUILT_AT")"
    echo "git_sha: $(git rev-parse HEAD)"
    echo "git_dirty: $([ -n "$DIRTY" ] && echo true || echo false)"
    echo "host: $(hostname)"
    echo "builder: $(whoami)"
) > "$STAGE/MANIFEST"

# ── Tar + compress ──────────────────────────────────────────────────────────
mkdir -p dist
OUT="dist/bpt-${VERSION}.tar.gz"
tar -czf "$OUT" \
    --owner=0 --group=0 \
    -C "$STAGE_ROOT" "bpt-${VERSION}"

echo ""
echo "=== Packaged: $OUT ($(du -h "$OUT" | cut -f1)) ==="
echo ""
echo "Top-level contents:"
tar -tzf "$OUT" | head -8
echo "..."
echo "Total entries: $(tar -tzf "$OUT" | wc -l)"
