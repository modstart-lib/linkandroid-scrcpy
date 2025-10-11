#!/bin/bash

# prepare
# brew install --cask inkscape

echo "Convert icon"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT=$(realpath "${DIR}/..")
echo "PROJECT_ROOT: ${PROJECT_ROOT}"

path_svg="${PROJECT_ROOT}/scripts/logo.svg"

path_build_svg=
path_build_ico="${PROJECT_ROOT}/app/data/icon.ico"
path_build_png="${PROJECT_ROOT}/app/data/icon.png"

rm -rfv "${PROJECT_ROOT}/app/data/icon.svg"
cp -a "${path_svg}" "${PROJECT_ROOT}/app/data/icon.svg"
rm -rfv "${path_build_ico}"
rm -rfv "${path_build_png}"
inkscape "${path_svg}" --export-type=png --export-filename="${path_build_png}" -w 256 -h 256
magick "${path_build_png}" -define icon:auto-resize=256,48,32,16 "${path_build_ico}"
