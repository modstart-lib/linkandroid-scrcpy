#!/usr/bin/env bash
set -ex
. $(dirname ${BASH_SOURCE[0]})/_init "$@"

ARCH=$(uname -m)
mkdir -p "$INSTALL_DIR/adb-linux"

if [[ "$ARCH" == aarch64 || "$ARCH" == arm64 ]]; then
    # Google does not provide prebuilt ARM64 platform-tools,
    # use system adb (installed via apt in CI workflow)
    if command -v adb &>/dev/null; then
        echo "Using system adb at $(which adb)"
        cp "$(which adb)" "$INSTALL_DIR/adb-linux/adb"
        chmod +x "$INSTALL_DIR/adb-linux/adb"
    else
        echo "ERROR: adb not found. Install it with: sudo apt install adb" >&2
        exit 1
    fi
else
    VERSION=37.0.0
    URL="https://dl.google.com/android/repository/platform-tools_r$VERSION-linux.zip"
    SHA256SUM=198ae156ab285fa555987219af237b31102fefe8b9d2bc274708a8d4f2865a07
    PROJECT_DIR="platform-tools-$VERSION-linux"
    FILENAME="$PROJECT_DIR.zip"

    cd "$SOURCES_DIR"

    if [[ -d "$PROJECT_DIR" ]]
    then
        echo "$PWD/$PROJECT_DIR" found
    else
        get_file "$URL" "$FILENAME" "$SHA256SUM"
        mkdir -p "$PROJECT_DIR"
        cd "$PROJECT_DIR"
        ZIP_PREFIX=platform-tools
        unzip "../$FILENAME" "$ZIP_PREFIX"/adb
        mv "$ZIP_PREFIX"/* .
        rmdir "$ZIP_PREFIX"
    fi

    cd "$INSTALL_DIR/adb-linux"
    cp -r "$SOURCES_DIR/$PROJECT_DIR"/. "$INSTALL_DIR/adb-linux/"
fi
