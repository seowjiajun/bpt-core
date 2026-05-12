#!/usr/bin/env bash
# build-public-mirror.sh — Produce a public-facing snapshot of this repo.
#
# The private repo (this one) stays intact with all strategies + tuned
# configs + sensitive operator state. This script writes a *scrubbed*
# snapshot to a sibling directory, suitable for force-pushing to a
# public GitHub remote as a portfolio artefact.
#
# What's dropped:
#   - Strategy classes with tuned alpha (AS, FundingArb, HMM, Momentum,
#     OFI, RegimeSwitch, ShortVol, VwapReversion). PassiveMaker stays
#     as a textbook framework demo.
#   - Their config TOMLs and instance wiring (the parameter values +
#     tuning narratives ARE the edge worth protecting).
#   - Env file examples that reference dropped strategies.
#   - Local-only state (`bin/`, `logs/`, working backups).
#
# What's regenerated:
#   - `bpt-strategy/src/strategy/strategy_factory.cpp` — slimmed to
#     register only public strategies (PassiveMaker).
#   - `LICENSE` (MIT).
#   - `README.md` (portfolio-facing version).
#
# What's scrubbed in-place via sed:
#   - Real wallet addresses (replaced with anonymised placeholders).
#   - Personal filesystem paths (/home/<user> → ${HOME}).
#   - Personal ownership tags ("Owner = X" → "Owner = "trading"").
#
# Usage:
#   scripts/build-public-mirror.sh [OUT_DIR]
#
# Default OUT_DIR is the sibling `bpt-core-public/`. The script clobbers
# that directory; everything in it is regenerated from scratch each run.
#
# After running:
#   cd $OUT_DIR && bazel build //... && bazel test //...
#   git init && git add . && git commit -m "initial public snapshot"
#   git remote add origin git@github.com:<you>/bpt-core.git
#   git push -u origin main --force   # force on subsequent runs

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/../bpt-core-public}"

# ── Public author identity ─────────────────────────────────────────────────
# These end up in LICENSE + README; export PUBLIC_AUTHOR / PUBLIC_AUTHOR_HANDLE
# to override. Used purely to fill placeholders — no commit author rewriting
# happens here (mirror is initialised with a fresh git history).
PUBLIC_AUTHOR="${PUBLIC_AUTHOR:-Jia Jun Seow}"
PUBLIC_AUTHOR_HANDLE="${PUBLIC_AUTHOR_HANDLE:-jseow}"
PUBLIC_YEAR="$(date +%Y)"

# ── Private file list (paths relative to repo root) ────────────────────────
# These are dropped from the mirror entirely. Anything outside this list
# survives, modulo the in-place scrub below.
PRIVATE_PATHS=(
    # Tuned strategy implementations.
    "bpt-strategy/include/strategy/strategy/avellaneda_stoikov_strategy.h"
    "bpt-strategy/src/strategy/avellaneda_stoikov_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/funding_arb_strategy.h"
    "bpt-strategy/src/strategy/funding_arb_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/hmm_strategy.h"
    "bpt-strategy/src/strategy/hmm_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/momentum_strategy.h"
    "bpt-strategy/src/strategy/momentum_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/ofi_strategy.h"
    "bpt-strategy/src/strategy/ofi_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/regime_switch_strategy.h"
    "bpt-strategy/src/strategy/regime_switch_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/short_vol_strategy.h"
    "bpt-strategy/src/strategy/short_vol_strategy.cpp"
    "bpt-strategy/include/strategy/strategy/vwap_reversion_strategy.h"
    "bpt-strategy/src/strategy/vwap_reversion_strategy.cpp"
    # Tuned param configs.
    "bpt-strategy/config/strategies/avellaneda_stoikov.backtest-xmr.toml"
    "bpt-strategy/config/strategies/avellaneda_stoikov.backtest.toml"
    "bpt-strategy/config/strategies/avellaneda_stoikov.hyperliquid.toml"
    "bpt-strategy/config/strategies/avellaneda_stoikov.toml"
    "bpt-strategy/config/strategies/avellaneda_stoikov.zec.backtest.toml"
    "bpt-strategy/config/strategies/funding_arb.hyperliquid.toml"
    "bpt-strategy/config/strategies/funding_arb.toml"
    "bpt-strategy/config/strategies/hmm.toml"
    "bpt-strategy/config/strategies/momentum.backtest.toml"
    "bpt-strategy/config/strategies/momentum.toml"
    "bpt-strategy/config/strategies/ofi.backtest.toml"
    "bpt-strategy/config/strategies/ofi.hl-live.toml"
    "bpt-strategy/config/strategies/ofi.hyperliquid.toml"
    "bpt-strategy/config/strategies/ofi.okx.toml"
    "bpt-strategy/config/strategies/regime_switch.backtest.toml"
    "bpt-strategy/config/strategies/regime_switch.toml"
    "bpt-strategy/config/strategies/short_vol.deribit.toml"
    "bpt-strategy/config/strategies/short_vol.okx.toml"
    "bpt-strategy/config/strategies/vwap_reversion.swap.toml"
    # Instance configs that reference private strategies.
    "bpt-strategy/config/avellaneda_stoikov.backtest-xmr.toml"
    "bpt-strategy/config/avellaneda_stoikov.backtest.toml"
    "bpt-strategy/config/avellaneda_stoikov.qa-hyperliquid.toml"
    "bpt-strategy/config/avellaneda_stoikov.qa-okx.toml"
    "bpt-strategy/config/avellaneda_stoikov.zec.backtest.toml"
    "bpt-strategy/config/fenrir.qa-okx.toml"
    "bpt-strategy/config/funding_arb.qa-hyperliquid.toml"
    "bpt-strategy/config/funding_arb.qa-okx.toml"
    "bpt-strategy/config/hmm.qa-okx.toml"
    "bpt-strategy/config/momentum.backtest.toml"
    "bpt-strategy/config/momentum.qa-okx.toml"
    "bpt-strategy/config/ofi.backtest.toml"
    "bpt-strategy/config/ofi.hl-live.toml"
    "bpt-strategy/config/ofi.qa-hyperliquid.toml"
    "bpt-strategy/config/ofi.qa-okx.toml"
    "bpt-strategy/config/regime_switch.backtest.toml"
    "bpt-strategy/config/regime_switch.qa-okx.toml"
    "bpt-strategy/config/short_vol.qa-deribit.toml"
    "bpt-strategy/config/short_vol.qa-okx.toml"
    "bpt-strategy/config/vwap_reversion.backtest.toml"
    "bpt-strategy/config/vwap_reversion.qa-okx.toml"
    # Tests tied to private strategies.
    "bpt-strategy/tests/unit/test_short_vol.cpp"
    "bpt-strategy/tests/unit/test_ofi.cpp"
    "bpt-strategy/tests/unit/test_avellaneda_stoikov.cpp"
    "bpt-strategy/tests/unit/test_funding_arb.cpp"
    # Env files referencing private strategies (operator-private anyway,
    # belt-and-braces — these are gitignored but the .example variants
    # often leak through. Keep only the PassiveMaker example.).
    "deploy/env/dev-hyperliquid-funding-arb.env.example"
)

# ── Exclude-from-rsync globs (always-drop patterns) ────────────────────────
RSYNC_EXCLUDES=(
    --exclude=".git/"
    --exclude="bazel-*"
    --exclude="build/"
    --exclude="build-cmake/"
    --exclude="bpt-*/bin/"
    --exclude="dashboard/*/bin/"
    --exclude="logs/"
    --exclude="*.log"
    --exclude="*.pid"
    --exclude=".env"
    --exclude="*.env"
    --exclude="*.env.bak"
    --exclude="*.bak"
    --exclude="vcpkg/"
    --exclude="node_modules/"
    --exclude="dashboard/frontend/dist/"
    --exclude="messages/build/"
    --exclude="transport/aeron/build/"
    --exclude="transport/aeron/.gradle/"
    --exclude=".vscode/"
    --exclude=".idea/"
    --exclude=".claude/"
    --exclude="research/"
    --exclude="RESEARCH_*"
    --exclude=".DS_Store"
    --exclude="__pycache__/"
    --exclude="*.pyc"
    --exclude=".pytest_cache/"
    --exclude=".venv/"
    --exclude="infra/terraform/**/.terraform/"
    --exclude="infra/terraform/**/terraform.tfstate*"
    --exclude="infra/terraform/**/*.tfvars"
)

echo "═══ Building public mirror ═══"
echo "Source:      $REPO_ROOT"
echo "Destination: $OUT_DIR"
echo

# ── 1. Wipe + copy ────────────────────────────────────────────────────────
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
rsync -a "${RSYNC_EXCLUDES[@]}" "$REPO_ROOT/" "$OUT_DIR/"

# ── 2. Drop explicit private paths ────────────────────────────────────────
echo "── Dropping ${#PRIVATE_PATHS[@]} private paths ──"
for p in "${PRIVATE_PATHS[@]}"; do
    if [ -e "$OUT_DIR/$p" ]; then
        rm -f "$OUT_DIR/$p"
    fi
done

# Clean up any now-empty directories left behind.
find "$OUT_DIR" -type d -empty -delete 2>/dev/null || true

# ── 3. Scrub sensitive strings in-place ───────────────────────────────────
echo "── Scrubbing sensitive strings ──"
# Wallet addresses — replace real ones with anonymised placeholder.
find "$OUT_DIR" -type f \( -name "*.toml" -o -name "*.cpp" -o -name "*.h" -o -name "*.md" \) \
    -exec sed -i 's|0x87b8D67Dd5B4aF939F6c53b18a711BF2654c4d9d|0x0000000000000000000000000000000000000001|g' {} +

# Personal home paths in deploy scripts + env templates.
find "$OUT_DIR" -type f \( -name "*.sh" -o -name "*.env*" -o -name "*.service" \) \
    -exec sed -i 's|/home/jseow|${HOME}|g' {} +

# Personal ownership tag in terraform.
find "$OUT_DIR/infra" -type f -name "*.tf" \
    -exec sed -i 's|Owner *= *"jseow"|Owner     = "trading"|g' {} + 2>/dev/null || true

# nvm path in generate-units (varies per machine).
find "$OUT_DIR/deploy" -type f -name "*.sh" \
    -exec sed -i 's|/home/jseow/.nvm/versions/node/v20.20.1/bin/node|/usr/bin/env node|g' {} +

# ── 4. Patch bpt-strategy/BUILD to drop private strategy sources ──────────
echo "── Patching bpt-strategy/BUILD ──"
PRIVATE_BUILD_SRCS=(
    "src/strategy/avellaneda_stoikov_strategy.cpp"
    "src/strategy/funding_arb_strategy.cpp"
    "src/strategy/hmm_filter.cpp"
    "src/strategy/hmm_strategy.cpp"
    "src/strategy/momentum_strategy.cpp"
    "src/strategy/ofi_strategy.cpp"
    "src/strategy/regime_switch_strategy.cpp"
    "src/strategy/short_vol_strategy.cpp"
    "src/strategy/vwap_reversion_strategy.cpp"
    "tests/unit/test_avellaneda_stoikov.cpp"
    "tests/unit/test_funding_arb.cpp"
    "tests/unit/test_short_vol.cpp"
    "tests/unit/test_ofi.cpp"
)
for src in "${PRIVATE_BUILD_SRCS[@]}"; do
    # Use # as the delimiter so the / in the source path doesn't conflict.
    sed -i "\#\"${src}\"#d" "$OUT_DIR/bpt-strategy/BUILD"
done

# ── 5. Regenerate strategy_factory.cpp (public-only) ──────────────────────
echo "── Regenerating public strategy_factory.cpp ──"
cat > "$OUT_DIR/bpt-strategy/src/strategy/strategy_factory.cpp" <<'CPP_EOF'
#include "strategy/strategy/strategy_factory.h"

#include "strategy/md/md_client.h"
#include "strategy/order/i_order_gateway_client.h"
#include "strategy/order/order_manager.h"
#include "strategy/strategy/passive_maker_strategy.h"

#include <fmt/format.h>
#include <stdexcept>

namespace bpt::strategy::strategy {

// Public mirror factory: only PassiveMakerStrategy is registered.
// The private repo extends this list with tuned strategies (AS, FundingArb,
// OFI, HMM, RegimeSwitch, Momentum, VwapReversion, ShortVol). See README
// for the rationale of why the framework is open-source but the tuned
// implementations and their config narratives are not.
std::unique_ptr<IStrategy> StrategyFactory::create(const config::EngineConfig& cfg,
                                                   refdata::IRefdataClient& refdata,
                                                   md::IMdClient* md,
                                                   order::OrderManager* order_mgr,
                                                   vol::VolSurfaceClient* /*vol_client*/) {
    const std::string& type = cfg.strategy.type;

    bpt::common::log::info("[StrategyFactory] Instantiating strategy type: {}", type);

    if (type == "PassiveMakerStrategy") {
        return std::make_unique<PassiveMakerStrategy>(cfg.correlation_id, cfg.strategy, refdata, md, order_mgr);
    }

    throw std::invalid_argument(fmt::format("Unknown strategy type: {}", type));
}

}  // namespace bpt::strategy::strategy
CPP_EOF

# ── 5. LICENSE (MIT) ──────────────────────────────────────────────────────
echo "── Writing LICENSE ──"
cat > "$OUT_DIR/LICENSE" <<LICENSE_EOF
MIT License

Copyright (c) ${PUBLIC_YEAR} ${PUBLIC_AUTHOR}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
LICENSE_EOF

# ── 6. README.public.md → README.md ───────────────────────────────────────
# The public mirror ships its own README pitched at recruiters. If the
# template exists alongside this script, use it; otherwise leave the
# existing README in place.
PUBLIC_README_TEMPLATE="$REPO_ROOT/scripts/public-mirror/README.md.tmpl"
if [ -f "$PUBLIC_README_TEMPLATE" ]; then
    echo "── Installing public README ──"
    sed -e "s|@@PUBLIC_AUTHOR@@|${PUBLIC_AUTHOR}|g" \
        -e "s|@@PUBLIC_AUTHOR_HANDLE@@|${PUBLIC_AUTHOR_HANDLE}|g" \
        -e "s|@@PUBLIC_YEAR@@|${PUBLIC_YEAR}|g" \
        "$PUBLIC_README_TEMPLATE" > "$OUT_DIR/README.md"
fi

echo
echo "═══ Mirror built at $OUT_DIR ═══"
echo
echo "Next steps:"
echo "  cd $OUT_DIR"
echo "  bazel build //...                    # verify it compiles"
echo "  bazel test //...                     # verify tests pass"
echo "  git init && git add . && git commit -m 'initial public snapshot'"
echo "  git remote add origin git@github.com:${PUBLIC_AUTHOR_HANDLE}/bpt-core.git"
echo "  git push -u origin main"
