#!/bin/bash
set -ex

case "$1" in
    32)
        WINXX=win32
        ;;
    64)
        WINXX=win64
        ;;
    *)
        echo "ERROR: $0 must be called with one argument: 32 or 64" >&2
        exit 1
        ;;
esac

cd "$(dirname ${BASH_SOURCE[0]})"
. build_common
cd .. # root project dir

WINXX_BUILD_DIR="$WORK_DIR/build-$WINXX"

app/deps/adb_windows.sh
app/deps/sdl.sh $WINXX cross static
app/deps/dav1d.sh $WINXX cross static
app/deps/ffmpeg.sh $WINXX cross static
app/deps/libusb.sh $WINXX cross static
app/deps/libwebsockets.sh $WINXX cross static

DEPS_INSTALL_DIR="$PWD/app/deps/work/install/$WINXX-cross-static"
ADB_INSTALL_DIR="$PWD/app/deps/work/install/adb-windows"

rm -rf "$WINXX_BUILD_DIR"
meson setup "$WINXX_BUILD_DIR" \
    --pkg-config-path="$DEPS_INSTALL_DIR/lib/pkgconfig" \
    -Dc_args="-I$DEPS_INSTALL_DIR/include" \
    -Dc_link_args="-L$DEPS_INSTALL_DIR/lib" \
    --cross-file=cross_$WINXX.txt \
    --buildtype=release \
    --strip \
    -Db_lto=true \
    -Dcompile_server=false \
    -Dportable=true \
    -Dstatic=true
ninja -C "$WINXX_BUILD_DIR"

# Group intermediate outputs into a 'dist' directory
mkdir -p "$WINXX_BUILD_DIR/dist"
cp "$WINXX_BUILD_DIR"/app/scrcpy.exe "$WINXX_BUILD_DIR/dist/"
cp app/data/scrcpy-console.bat "$WINXX_BUILD_DIR/dist/"
cp app/data/scrcpy-noconsole.vbs "$WINXX_BUILD_DIR/dist/"
cp app/data/icon.png "$WINXX_BUILD_DIR/dist/"
cp app/data/open_a_terminal_here.bat "$WINXX_BUILD_DIR/dist/"
# Copy DLL files if they exist (they won't exist for static builds)
if ls "$DEPS_INSTALL_DIR"/bin/*.dll 1> /dev/null 2>&1; then
    cp "$DEPS_INSTALL_DIR"/bin/*.dll "$WINXX_BUILD_DIR/dist/"
fi
cp -r "$ADB_INSTALL_DIR"/. "$WINXX_BUILD_DIR/dist/"
