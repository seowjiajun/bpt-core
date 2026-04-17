#!/bin/bash
set -e

SCRIPT_DIR="$(dirname "$0")"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
JAR_FILE=$(find "$PROJECT_DIR/build/libs" -name "*-all.jar" -type f | head -1)

if [ -z "$JAR_FILE" ]; then
    echo "Error: Shadow JAR not found in $PROJECT_DIR/build/libs"
    echo "Please run './gradlew shadowJar' first."
    exit 1
fi

echo "Starting application from $JAR_FILE"
java --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
     --add-opens java.base/java.nio=ALL-UNNAMED \
     -jar "$JAR_FILE" "$@"
