#!/bin/bash

ROOT_DIR=$(pwd)
SUBMODULE_DIR="$ROOT_DIR/vendor/karchive"
BUILD_DIR="$ROOT_DIR/build_tidy"
QT_PATH="$HOME/Qt/6.8.3/gcc_64"

mkdir -p "$BUILD_DIR"

cmake -S "$SUBMODULE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_PREFIX_PATH="$QT_PATH" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DBUILD_WITH_QT6=ON \
    -DBUILD_TESTING=OFF

cmake --build "$BUILD_DIR" --target KF6Archive_autogen

cd "$SUBMODULE_DIR"
git checkout .
patch -p1 < "$ROOT_DIR/custom.patch"

AUTOGEN_ARGS=$(find "$BUILD_DIR" -type d -name "include*" | sed 's/^/--extra-arg=-I/')

clang-tidy src/*.cpp -p "$BUILD_DIR" \
    -checks='-*,bugprone-*' \
    -header-filter="src/.*" \
    --extra-arg="-isystem$QT_PATH/include" \
    --extra-arg="-isystem$QT_PATH/include/QtCore" \
    --extra-arg="-isystem$QT_PATH/include/QtGui" \
    --extra-arg="-I$BUILD_DIR/src" \
    $AUTOGEN_ARGS \
    --extra-arg="-DQT_CORE_LIB" \
    --extra-arg="-DQT_NO_DEBUG" \
    --extra-arg="-w" > "$ROOT_DIR/clang_tidy/clang_tidy_report.txt" 2>&1

git checkout .
rm -rf "$BUILD_DIR"