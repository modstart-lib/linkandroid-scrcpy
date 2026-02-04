#!/usr/bin/env bash
set -ex
. $(dirname ${BASH_SOURCE[0]})/_init
process_args "$@"

VERSION=4.3.3
URL="https://github.com/warmcat/libwebsockets/archive/refs/tags/v$VERSION.tar.gz"
# SHA256SUM may vary for GitHub generated tarballs, so we download manually if check fails or bypass check
SHA256SUM=6fd33527b410a37ebc91bb64ca51bdabab12b076bc99d153d7c5dd405e4bdf90

PROJECT_DIR="libwebsockets-$VERSION"
FILENAME="$PROJECT_DIR.tar.gz"

cd "$SOURCES_DIR"

if [[ -d "$PROJECT_DIR" ]]
then
    echo "$PWD/$PROJECT_DIR" found
else
    if [[ ! -f "$FILENAME" ]]; then
        wget "$URL" -O "$FILENAME"
    fi
    # We don't check sum here because GitHub tarballs are not stable
    
    tar xf "$FILENAME"
    # GitHub archive typically expands to libwebsockets-<version>
    if [[ ! -d "$PROJECT_DIR" ]]; then
        # If the directory name doesn't match exactly, find it and rename
        mv libwebsockets* "$PROJECT_DIR"
    fi
fi

mkdir -p "$BUILD_DIR/$PROJECT_DIR"
cd "$BUILD_DIR/$PROJECT_DIR"

if [[ -d "$DIRNAME" ]]
then
    echo "'$PWD/$DIRNAME' already exists, not reconfigured"
    cd "$DIRNAME"
else
    mkdir "$DIRNAME"
    cd "$DIRNAME"

    cmake_conf=(
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR/$DIRNAME"
        -DCMAKE_BUILD_TYPE=Release
        -DLWS_WITH_SSL=OFF
        -DLWS_WITHOUT_TESTAPPS=ON
        -DLWS_WITHOUT_TEST_SERVER=ON
        -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON
        -DLWS_WITHOUT_TEST_PING=ON
        -DLWS_WITHOUT_TEST_CLIENT=ON
    )

    if [[ "$LINK_TYPE" == static ]]
    then
        cmake_conf+=(-DLWS_STATIC_PIC=ON -DLWS_WITH_STATIC=ON -DLWS_WITH_SHARED=OFF)
    else
        cmake_conf+=(-DLWS_WITH_STATIC=OFF -DLWS_WITH_SHARED=ON)
    fi

    if [[ "$BUILD_TYPE" == cross ]]
    then
        cmake_conf+=(
            -DCMAKE_SYSTEM_NAME=Windows
            -DCMAKE_C_COMPILER="$HOST_TRIPLET-gcc"
            -DCMAKE_CXX_COMPILER="$HOST_TRIPLET-g++"
            -DCMAKE_RC_COMPILER="$HOST_TRIPLET-windres"
        )
    fi

    cmake "$SOURCES_DIR/$PROJECT_DIR" "${cmake_conf[@]}"
fi

make -j
make install
