#!/bin/bash
set -e

# Get the directory of the script
SCRIPT_DIR="$(dirname "$0")"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "Navigating to project directory: $PROJECT_DIR"
cd "$PROJECT_DIR"

if [ -f "./gradlew" ]; then
    echo "Cleaning and building project..."
    ./gradlew clean build
else
    echo "Error: gradlew not found in run directory. Please run scripts/bootstrap_build.sh first."
    exit 1
fi

echo "Clean build complete."
