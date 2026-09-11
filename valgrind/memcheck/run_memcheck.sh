#!/usr/bin/env bash
#
# Runs Valgrind memcheck over the test suites, capturing leaks and
# uninitialised-memory use that AddressSanitizer structurally cannot see.
#
# Reproduce with:   ./valgrind/memcheck/run_memcheck.sh
# Requires:         valgrind >= 3.20, cmake, ninja, Qt >= 6.8,
#                   extra-cmake-modules >= 6.21
#
# Outputs (committed):
#   valgrind/memcheck/<suite>.log   full memcheck output per suite
#   valgrind/memcheck/summary.txt   error counts per suite
#
# Why memcheck AND AddressSanitizer? They cover different blind spots. ASan only
# sees code it recompiled, so it is blind inside the system compression
# libraries (zlib, bzip2, liblzma, zstd) and cannot detect uninitialised reads
# at all. Memcheck needs no recompilation and does detect uninitialised reads.
# The build here is therefore an ordinary (uninstrumented) build, NOT an ASan
# build.
#
# No suppression file is used. The findings are few and each is triaged
# individually in TRIAGE.md; a broad suppression (the only kind valgrind could
# generate here, because the noisy stacks are unsymbolised) would risk hiding
# real defects, and one of the findings below is a real KArchive defect.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-memcheck"
OUT_DIR="$ROOT_DIR/valgrind/memcheck"
QT_LIB="${QT_PATH:-$HOME/Qt/6.8.3/gcc_64}/lib"

if [ ! -e "$ROOT_DIR/vendor/karchive/src/karchive.cpp" ]; then
    echo "error: vendor/karchive is empty. Run: git submodule update --init" >&2
    exit 1
fi

echo ">> configuring $BUILD_DIR (uninstrumented, -O1 -g)"
CXXFLAGS="-O1 -g -fno-omit-frame-pointer" \
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja >/dev/null
echo ">> building"
cmake --build "$BUILD_DIR" >/dev/null

export LD_LIBRARY_PATH="$BUILD_DIR/bin:$QT_LIB:${LD_LIBRARY_PATH:-}"
SUITES=(vs_robustnesstest vs_roundtriptest karchivetest)

{
    echo "Valgrind memcheck run -- KArchive 633dc09 (no suppressions; see TRIAGE.md)"
    echo
} > "$OUT_DIR/summary.txt"

for suite in "${SUITES[@]}"; do
    bin="$BUILD_DIR/bin/$suite"
    [ -x "$bin" ] || { echo ">> skip $suite (not built)"; continue; }
    echo ">> memcheck: $suite"
    valgrind --tool=memcheck \
             --leak-check=full \
             --show-leak-kinds=definite,indirect \
             --track-origins=yes \
             --num-callers=25 \
             "$bin" > "$OUT_DIR/$suite.log" 2>&1
    summary=$(grep "ERROR SUMMARY" "$OUT_DIR/$suite.log" | head -1 | sed 's/^==[0-9]*== //')
    lost=$(grep "definitely lost:" "$OUT_DIR/$suite.log" | head -1 | sed 's/^==[0-9]*== *//')
    printf '%-22s %s | %s\n' "$suite" "$summary" "$lost" >> "$OUT_DIR/summary.txt"
done

echo
cat "$OUT_DIR/summary.txt"
