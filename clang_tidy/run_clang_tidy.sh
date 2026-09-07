#!/usr/bin/env bash
#
# Runs clang-tidy over KArchive's library sources and writes the raw report to
# clang_tidy/clang_tidy_report.txt.
#
# Reproduce with:   ./clang_tidy/run_clang_tidy.sh
# Requires:         clang-tidy, cmake, ninja, Qt >= 6.8, extra-cmake-modules >= 6.21
#
# The analysed submodule is never modified. An earlier version of this script
# applied custom.patch and then ran `git checkout .` inside vendor/karchive to
# undo it; that is gone, because the wrapper CMakeLists.txt now builds the
# submodule directly and a stray `git checkout .` would silently discard any
# local work in there.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-tidy"
SRC_DIR="$ROOT_DIR/vendor/karchive/src"
REPORT="$ROOT_DIR/clang_tidy/clang_tidy_report.txt"

if [ ! -e "$SRC_DIR/karchive.cpp" ]; then
    echo "error: vendor/karchive is empty. Run: git submodule update --init" >&2
    exit 1
fi

# Configure only to obtain compile_commands.json plus the generated headers the
# sources include (karchive_export.h, karchive_version.h, moc output).
echo ">> configuring $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DBUILD_TESTING=OFF >/dev/null

echo ">> generating headers"
cmake --build "$BUILD_DIR" --target KF6Archive_autogen >/dev/null

# NOTE: -checks here overrides the .clang-tidy file, so only bugprone-* is
# currently applied even though .clang-tidy also lists modernize-*,
# performance-* and several readability checks. Reconciling the two (and
# tightening header-filter so generated headers stop dominating the output)
# is the first task of the static-analysis stage.
echo ">> running clang-tidy over $(ls "$SRC_DIR"/*.cpp | wc -l) source files"
clang-tidy "$SRC_DIR"/*.cpp \
    -p "$BUILD_DIR" \
    -checks='-*,bugprone-*' \
    --header-filter='vendor/karchive/src/.*' \
    --extra-arg='-w' \
    > "$REPORT" 2>&1 || true

echo ">> report written to ${REPORT#"$ROOT_DIR"/} ($(wc -l < "$REPORT") lines)"
