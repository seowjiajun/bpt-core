#!/bin/bash
# Wrapper script to run the application via Gradle and forward arguments
SCRIPT_DIR="$(dirname "$0")"

if [ -z "$1" ]; then
    "$SCRIPT_DIR/../gradlew" -p "$SCRIPT_DIR/.." run
else
    "$SCRIPT_DIR/../gradlew" -p "$SCRIPT_DIR/.." run --args="$*"
fi
