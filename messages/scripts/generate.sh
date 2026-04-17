#!/usr/bin/env bash
# Regenerates C++ SBE encoders/decoders from schema/bifrost-protocol.xml.
# Prerequisites: JDK 17+, tools/sbe-all-*.jar  (run download-sbe-tool.sh first)
#
# Usage:
#   ./scripts/generate.sh
#
# Outputs go to generated/cpp/ and are committed to the repo so that
# consumers (Sindri, TradingBot) need no build-time code-gen step.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCHEMA="${REPO_ROOT}/schema/bifrost-protocol.xml"
OUT_DIR="${REPO_ROOT}/generated/cpp"
TOOLS_DIR="${REPO_ROOT}/tools"

SBE_JAR="$(ls "${TOOLS_DIR}"/sbe-all-*.jar 2>/dev/null | sort -V | tail -1)"
if [[ -z "${SBE_JAR}" ]]; then
  echo "[error] No sbe-all-*.jar found in ${TOOLS_DIR}."
  echo "        Run:  ./scripts/download-sbe-tool.sh"
  exit 1
fi

echo "[sbe] Using: ${SBE_JAR}"
echo "[sbe] Schema: ${SCHEMA}"
echo "[sbe] Output: ${OUT_DIR}"

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}"

java \
  -Dsbe.output.dir="${OUT_DIR}" \
  -Dsbe.target.language=cpp \
  -Dsbe.cpp.namespaces.collapse=false \
  -Dsbe.xinclude.aware=true \
  -jar "${SBE_JAR}" \
  "${SCHEMA}"

echo "[sbe] Generation complete."
echo "[sbe] Files written to ${OUT_DIR}:"
find "${OUT_DIR}" -type f | sort | sed 's|^|  |'
