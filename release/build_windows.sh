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
    -Dc_link_args="-L$DEPS_INSTALL_DIR/lib -static-libgcc -static-libstdc++ -Wl,-Bstatic,--whole-archive -lwinpthread -Wl,--no-whole-archive" \
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
cp app/data/font.ttf "$WINXX_BUILD_DIR/dist/"
cp app/data/open_a_terminal_here.bat "$WINXX_BUILD_DIR/dist/"
# Copy DLL files if they exist (they won't exist for static builds)
if ls "$DEPS_INSTALL_DIR"/bin/*.dll 1> /dev/null 2>&1; then
    cp "$DEPS_INSTALL_DIR"/bin/*.dll "$WINXX_BUILD_DIR/dist/"
fi

# Copy MinGW runtime DLLs if still needed (fallback for dynamic linking)
# Find MinGW bin directory
case "$1" in
    32)
        MINGW_BIN=$(dirname $(which i686-w64-mingw32-gcc) 2>/dev/null)
        ;;
    64)
        MINGW_BIN=$(dirname $(which x86_64-w64-mingw32-gcc) 2>/dev/null)
        ;;
esac

if [ -n "$MINGW_BIN" ]; then
    # Copy required MinGW runtime DLLs
    for dll in libwinpthread-1.dll libgcc_s_seh-1.dll libgcc_s_sjlj-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll; do
        if [ -f "$MINGW_BIN/$dll" ]; then
            cp "$MINGW_BIN/$dll" "$WINXX_BUILD_DIR/dist/" 2>/dev/null || true
        fi
    done
fi

cp -r "$ADB_INSTALL_DIR"/. "$WINXX_BUILD_DIR/dist/"
