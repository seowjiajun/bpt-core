#!/bin/bash
set -e

PROJECT_DIR="$(dirname "$0")/.."
GRADLE_VERSION="8.5"
GRADLE_URL="https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip"
TMP_DIR="/tmp/gradle-setup"

mkdir -p "$TMP_DIR"

# Prefer the Gradle wrapper if it already exists
if [ -f "$PROJECT_DIR/gradlew" ]; then
    echo "Gradle wrapper found. Skipping bootstrap."
    GRADLE_BIN="$PROJECT_DIR/gradlew"
elif ! command -v gradle &> /dev/null; then
    echo "Gradle not found. Downloading version ${GRADLE_VERSION}..."
    if [ ! -d "$TMP_DIR/gradle-$GRADLE_VERSION" ]; then
        if [ ! -f "$TMP_DIR/gradle.zip" ]; then
            if command -v curl &> /dev/null; then
                curl -fsSL -o "$TMP_DIR/gradle.zip" "$GRADLE_URL"
            elif command -v wget &> /dev/null; then
                wget -q -O "$TMP_DIR/gradle.zip" "$GRADLE_URL"
            else
                echo "ERROR: Neither curl nor wget is available. Please install one and retry."
                exit 1
            fi
        fi
        if command -v unzip &> /dev/null; then
            unzip -q "$TMP_DIR/gradle.zip" -d "$TMP_DIR"
        elif command -v python3 &> /dev/null; then
            python3 -m zipfile -e "$TMP_DIR/gradle.zip" "$TMP_DIR"
        else
            echo "ERROR: Neither unzip nor python3 is available. Please install one and retry."
            exit 1
        fi
    fi
    GRADLE_BIN="$TMP_DIR/gradle-$GRADLE_VERSION/bin/gradle"
    chmod +x "$GRADLE_BIN"
else
    GRADLE_BIN="gradle"
fi

echo "Using Gradle: $GRADLE_BIN"

cd "$PROJECT_DIR"

if [ ! -f "./gradlew" ]; then
    echo "Generating Gradle Wrapper..."
    "$GRADLE_BIN" wrapper
fi

echo "Building Project..."
./gradlew build

echo "Build Complete."
