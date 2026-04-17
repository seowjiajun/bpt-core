#!/usr/bin/env bash
# Downloads the Real Logic SBE all-in-one JAR from Maven Central.
# Run once before generate.sh.  The JAR is not checked in.

set -euo pipefail

SBE_VERSION="1.30.0"
JAR_NAME="sbe-all-${SBE_VERSION}.jar"
TOOLS_DIR="$(cd "$(dirname "$0")/../tools" && pwd)"
DEST="${TOOLS_DIR}/${JAR_NAME}"

if [[ -f "${DEST}" ]]; then
  echo "[sbe] ${JAR_NAME} already present — skipping download."
  exit 0
fi

MAVEN_URL="https://repo1.maven.org/maven2/uk/co/real-logic/sbe-all/${SBE_VERSION}/${JAR_NAME}"

echo "[sbe] Downloading ${JAR_NAME} from Maven Central..."
curl -fSL --progress-bar -o "${DEST}" "${MAVEN_URL}"
echo "[sbe] Saved to ${DEST}"
