#!/usr/bin/env bash
# Installs git hooks for this repo.  Run once after cloning.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HOOKS_DIR="${REPO_ROOT}/.git/hooks"

cat > "${HOOKS_DIR}/pre-commit" << 'EOF'
#!/usr/bin/env bash
# Regenerates SBE code and aborts the commit if generated/ has changed,
# forcing the developer to review and stage the new output.

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel)"

# Only run if the schema was staged in this commit.
if ! git diff --cached --name-only | grep -q "^schema/"; then
    exit 0
fi

echo "[pre-commit] schema change detected — regenerating SBE code..."
"${REPO_ROOT}/scripts/generate.sh"

if ! git diff --exit-code generated/ > /dev/null 2>&1; then
    echo ""
    echo "[pre-commit] generated/ has changed. Stage the new files and re-commit:"
    echo "  git add generated/"
    echo "  git commit"
    exit 1
fi

echo "[pre-commit] generated/ is up to date."
EOF

chmod +x "${HOOKS_DIR}/pre-commit"
echo "Installed pre-commit hook at ${HOOKS_DIR}/pre-commit"
