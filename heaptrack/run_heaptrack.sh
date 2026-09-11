#!/usr/bin/env bash
#
# Heap-profiles KArchive extracting a moderately large archive, in each writable
# format, with heaptrack (KDE's heap profiler).
#
# Reproduce with:   ./heaptrack/run_heaptrack.sh
# Requires:         heaptrack, g++, cmake, ninja, Qt >= 6.8,
#                   extra-cmake-modules >= 6.21, a built ./build
#
# Outputs (committed):
#   heaptrack/<format>.summary.txt   heaptrack_print summary per format
#   heaptrack/comparison.txt         peak heap / allocations side by side
# The raw heaptrack.*.zst traces are large and machine-specific; they are
# written to a temp dir, summarised, and discarded.
#
# heaptrack is one of the tools not covered in the exercises, and it is NOT a
# Valgrind tool, so it does not consume the one-Valgrind-tool budget memcheck
# spends -- we get heap profiling and memcheck both.
#
# Note: `heaptrack -o` tries to auto-open heaptrack_gui, which crashes in this
# environment (a snap/glibc clash unrelated to KArchive). --record-only avoids
# that; analysis is done separately with heaptrack_print.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
OUT_DIR="$ROOT_DIR/heaptrack"
QT_PATH="${QT_PATH:-$HOME/Qt/6.8.3/gcc_64}"
TRACE_DIR="$(mktemp -d)"
trap 'rm -rf "$TRACE_DIR"' EXIT

if [ ! -e "$BUILD_DIR/bin/libKF6Archive.so" ]; then
    echo ">> building project first"
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja >/dev/null
    cmake --build "$BUILD_DIR" >/dev/null
fi

echo ">> compiling extract driver"
g++ -std=c++20 -O2 -g "$OUT_DIR/extract_driver.cpp" -o "$TRACE_DIR/extract_driver" \
    -I"$ROOT_DIR/vendor/karchive/src" -I"$BUILD_DIR/vendor/karchive/src" \
    -isystem"$QT_PATH/include" -isystem"$QT_PATH/include/QtCore" \
    -L"$BUILD_DIR/bin" -lKF6Archive -L"$QT_PATH/lib" -lQt6Core

export LD_LIBRARY_PATH="$BUILD_DIR/bin:$QT_PATH/lib:${LD_LIBRARY_PATH:-}"

ENTRY_SIZE=${ENTRY_SIZE:-1048576}   # 1 MiB
ENTRY_COUNT=${ENTRY_COUNT:-64}      # -> 64 MiB payload
{
    echo "heaptrack: write then extract $ENTRY_COUNT x $((ENTRY_SIZE/1024/1024)) MiB entries (payload $((ENTRY_COUNT*ENTRY_SIZE/1024/1024)) MiB)"
    echo "target: KArchive 633dc09"
    echo
    printf '%-10s %12s %12s %14s %12s\n' "format" "peak-heap" "peak-RSS" "allocations" "leaked"
} > "$OUT_DIR/comparison.txt"

for fmt in tar tar.gz tar.zst zip 7z; do
    echo ">> heaptrack: $fmt"
    heaptrack --record-only -o "$TRACE_DIR/ht_$fmt" \
        "$TRACE_DIR/extract_driver" "$fmt" "$ENTRY_SIZE" "$ENTRY_COUNT" >/dev/null 2>&1
    trace=$(ls "$TRACE_DIR"/ht_$fmt*.zst 2>/dev/null | head -1)
    [ -z "$trace" ] && { echo "   (no trace for $fmt)"; continue; }

    heaptrack_print "$trace" 2>/dev/null > "$OUT_DIR/$fmt.summary.txt"

    get() { grep -iE "^$1" "$OUT_DIR/$fmt.summary.txt" | head -1 | grep -oE "[0-9.]+[KMGB]+" | head -1; }
    peak=$(get "peak heap memory consumption")
    rss=$(get "peak RSS")
    leak=$(get "total memory leaked")
    nall=$(grep -iE "^calls to allocation functions" "$OUT_DIR/$fmt.summary.txt" | head -1 | grep -oE "[0-9]+" | head -1)
    printf '%-10s %12s %12s %14s %12s\n' "$fmt" "${peak:-?}" "${rss:-?}" "${nall:-?}" "${leak:-?}" >> "$OUT_DIR/comparison.txt"
done

echo
cat "$OUT_DIR/comparison.txt"
