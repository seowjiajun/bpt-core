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

mkdir -p "$STAGE"/{bin/transport,share/schema,share/config-templates,share/instruments,scripts}

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
# Canonical instrument mappings (consumed at runtime by bpt-refdata).
# Only ship runtime JSONs — skip Bazel BUILD files / anything else.
for f in config/instruments/*.json; do
    [ -f "$f" ] && cp "$f" "$STAGE/share/instruments/"
done
# Strategy default / shared parameter templates (reference material for operators).
for f in bpt-strategy/config/strategies/*.toml; do
    [ -f "$f" ] && cp "$f" "$STAGE/share/config-templates/"
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
