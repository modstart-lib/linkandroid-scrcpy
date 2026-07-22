#!/usr/bin/env bash
set -ex
. $(dirname ${BASH_SOURCE[0]})/_init
process_args "$@"

# Build FreeType2 first (required by SDL3_ttf)
FREETYPE_VERSION=2.13.3
FREETYPE_URL="https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VERSION.tar.gz"
FREETYPE_SHA256=5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747

FREETYPE_PROJECT_DIR="freetype-$FREETYPE_VERSION"
FREETYPE_FILENAME="$FREETYPE_PROJECT_DIR.tar.gz"

cd "$SOURCES_DIR"

if [[ ! -d "$FREETYPE_PROJECT_DIR" ]]
then
    get_file "$FREETYPE_URL" "$FREETYPE_FILENAME" "$FREETYPE_SHA256"
    tar xf "$FREETYPE_FILENAME"
fi

mkdir -p "$BUILD_DIR/$FREETYPE_PROJECT_DIR"
cd "$BUILD_DIR/$FREETYPE_PROJECT_DIR"

export CFLAGS='-O2'
export CXXFLAGS="$CFLAGS"

if [[ ! -d "$DIRNAME" ]]
then
    mkdir "$DIRNAME"
    cd "$DIRNAME"
    
    conf=(
        --prefix="$INSTALL_DIR/$DIRNAME"
        --enable-static
        --disable-shared
        --without-harfbuzz
        --without-png
        --without-bzip2
        --without-brotli
    )
    
    if [[ "$BUILD_TYPE" == cross ]]
    then
        conf+=(
            --host="$HOST_TRIPLET"
        )
    fi
    
    "$SOURCES_DIR/$FREETYPE_PROJECT_DIR/configure" "${conf[@]}"
else
    cd "$DIRNAME"
fi

make -j
make install

# Build SDL3_ttf using CMake
SDL3_TTF_VERSION=3.2.2
SDL3_TTF_URL="https://github.com/libsdl-org/SDL_ttf/releases/download/release-$SDL3_TTF_VERSION/SDL3_ttf-$SDL3_TTF_VERSION.tar.gz"

SDL3_TTF_PROJECT_DIR="SDL3_ttf-$SDL3_TTF_VERSION"
SDL3_TTF_FILENAME="$SDL3_TTF_PROJECT_DIR.tar.gz"

cd "$SOURCES_DIR"

if [[ ! -d "$SDL3_TTF_PROJECT_DIR" ]]
then
    if [[ ! -f "$SDL3_TTF_FILENAME" ]]; then
        wget "$SDL3_TTF_URL" -O "$SDL3_TTF_FILENAME"
    fi
    tar xf "$SDL3_TTF_FILENAME"
fi

mkdir -p "$BUILD_DIR/$SDL3_TTF_PROJECT_DIR"
cd "$BUILD_DIR/$SDL3_TTF_PROJECT_DIR"

if [[ ! -d "$DIRNAME" ]]
then
    mkdir "$DIRNAME"
    cd "$DIRNAME"
    
    SDL3_DIR="$INSTALL_DIR/$DIRNAME"
    FREETYPE_DIR="$INSTALL_DIR/$DIRNAME"
    
    CMAKE_OPTS=(
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/$DIRNAME"
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_SHARED_LIBS=OFF
        -DSDL3TTF_HARFBUZZ=OFF
        -DSDL3TTF_FREETYPE=ON
        -DSDL3TTF_VENDORED=OFF
        -DSDL3TTF_SAMPLES=OFF
        -DCMAKE_PREFIX_PATH="$SDL3_DIR;$FREETYPE_DIR"
        -DFREETYPE_LIBRARY="$FREETYPE_DIR/lib/libfreetype.a"
        -DFREETYPE_INCLUDE_DIRS="$FREETYPE_DIR/include/freetype2"
    )
    
    if [[ "$BUILD_TYPE" == cross ]]
    then
        CMAKE_OPTS+=(
            -DCMAKE_SYSTEM_NAME=Windows
            -DCMAKE_C_COMPILER=${HOST_TRIPLET}-gcc
            -DCMAKE_CXX_COMPILER=${HOST_TRIPLET}-g++
            -DCMAKE_RC_COMPILER=${HOST_TRIPLET}-windres
            -DCMAKE_FIND_ROOT_PATH="$INSTALL_DIR/$DIRNAME"
            -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
            -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
            -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
        )
    fi
    
    cmake "$SOURCES_DIR/$SDL3_TTF_PROJECT_DIR" "${CMAKE_OPTS[@]}"
else
    cd "$DIRNAME"
fi

make -j
make install
