#!/usr/bin/env bash
#
# Builds and runs the standalone reproducers for the two defects described in
# unit_tests/FINDINGS.md.
#
# Reproduce with:   ./unit_tests/reproducers/build_and_run.sh
# Requires:         g++, Qt >= 6.8, and a completed build of the project
#                   (run `cmake -S . -B build -G Ninja && cmake --build build`
#                    first -- these link against build/lib/libKF6Archive.so)
#
# These are deliberately not part of the CTest suite: one of them crashes the
# process by design, and a crash cannot be caught by QTest.

set -uo pipefail   # deliberately not -e: a crashing reproducer is the point

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT_DIR/unit_tests/reproducers"
BUILD_DIR="$ROOT_DIR/build"
BIN_DIR="$(mktemp -d)"
trap 'rm -rf "$BIN_DIR"' EXIT

# ECM's KDECMakeSettings puts shared libraries next to the executables in bin/,
# but a plain CMake layout uses lib/. Accept either.
LIB=""
for candidate in "$BUILD_DIR/bin" "$BUILD_DIR/lib"; do
    if [ -e "$candidate/libKF6Archive.so" ]; then
        LIB="$candidate"
        break
    fi
done
if [ -z "$LIB" ]; then
    echo "error: libKF6Archive.so not found under $BUILD_DIR." >&2
    echo "       Build the project first: cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
fi

# Find Qt the same way the top-level CMakeLists.txt does.
QT_PATH="${QT_PATH:-}"
if [ -z "$QT_PATH" ]; then
    for candidate in "$HOME/Qt/6.8.3/gcc_64" "$HOME/Qt/6.10.1/gcc_64"; do
        [ -d "$candidate" ] && { QT_PATH="$candidate"; break; }
    done
fi
if [ -z "$QT_PATH" ]; then
    echo "error: no Qt found. Set QT_PATH to your Qt 6 prefix." >&2
    exit 1
fi

compile() {
    g++ -std=c++20 -fPIC -g "$HERE/$1.cpp" -o "$BIN_DIR/$1" \
        -I"$ROOT_DIR/vendor/karchive/src" \
        -I"$BUILD_DIR/vendor/karchive/src" \
        -isystem"$QT_PATH/include" \
        -isystem"$QT_PATH/include/QtCore" \
        -L"$LIB" -lKF6Archive \
        -L"$QT_PATH/lib" -lQt6Core
}

export LD_LIBRARY_PATH="$LIB:$QT_PATH/lib:${LD_LIBRARY_PATH:-}"
RCC="$ROOT_DIR/vendor/karchive/autotests/data/runtime_resource.rcc"

echo "=============================================================="
echo " Finding 1: KRcc crashes on a corrupted .rcc"
echo "=============================================================="
compile krcc_crash || exit 1
printf '%-28s %s\n' "input" "result"
for spec in "12 255" "15 0" "15 255" "20 255" "24 255" "63 0"; do
    set -- $spec
    # The subshell keeps bash's "Segmentation fault (core dumped)" job-control
    # message out of the output; the exit status still carries the signal.
    ( "$BIN_DIR/krcc_crash" "$RCC" "$1" "$2" >/dev/null 2>&1 ) 2>/dev/null
    rc=$?
    if [ $rc -ge 128 ]; then
        sig=$((rc - 128))
        name=$([ $sig -eq 7 ] && echo SIGBUS || echo "signal $sig")
        printf '%-28s *** CRASH (%s) ***\n' "offset $1 = $2" "$name"
    else
        printf '%-28s survived\n' "offset $1 = $2"
    fi
done

echo
echo "=============================================================="
echo " Finding 2: K7Zip corrupts non-ASCII filenames"
echo "=============================================================="
compile k7zip_filename_corruption || exit 1
"$BIN_DIR/k7zip_filename_corruption" 2>&1
