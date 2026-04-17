#\!/bin/bash
set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$TOOLS_DIR/.." && pwd)"

JAR=$(find "$PROJECT_DIR/build/libs" -name "*-all.jar" -type f 2>/dev/null | head -1)

if [ -z "$JAR" ]; then
    echo "Shadow JAR not found. Building..."
    "$PROJECT_DIR/gradlew" -p "$PROJECT_DIR" shadowJar -q
    JAR=$(find "$PROJECT_DIR/build/libs" -name "*-all.jar" -type f | head -1)
fi

# If first arg matches a .java file in this directory, run it as a single-file
# source program (Java 11+). No compilation step, no .class files produced.
FIRST="${1:-}"
JAVA_FILE="$TOOLS_DIR/$FIRST.java"

if [ -f "$JAVA_FILE" ]; then
    shift
    exec java \
        --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
        --add-opens java.base/java.nio=ALL-UNNAMED \
        -cp "$JAR" \
        "$JAVA_FILE" "$@"
else
    exec java \
        --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
        --add-opens java.base/java.nio=ALL-UNNAMED \
        -cp "$JAR:$TOOLS_DIR" \
        "$@"
fi
