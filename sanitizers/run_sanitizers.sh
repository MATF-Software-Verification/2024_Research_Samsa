#!/usr/bin/env bash
#
# Builds the whole project (KArchive + our test suites) with AddressSanitizer
# and UndefinedBehaviorSanitizer, then runs every suite under it.
#
# Reproduce with:   ./sanitizers/run_sanitizers.sh
# Requires:         clang or g++ with ASan/UBSan, cmake, ninja,
#                   Qt >= 6.8, extra-cmake-modules >= 6.21
#
# Instrumentation is injected from the top-level CMakeLists.txt via
# ANALYSIS_SANITIZE, so it reaches KArchive's own sources without patching them
# (the same mechanism the coverage build uses).
#
# Outputs (committed):
#   sanitizers/asan_ubsan.log   full combined output of the run
#   sanitizers/summary.txt      one line per suite: clean / errors
#
# ASan is also the runtime for the fuzzing stage (see ../afl): a fuzzer without
# a sanitizer only finds crashes that happen to segfault, whereas with ASan it
# finds the far larger set of memory errors that would otherwise pass silently.

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-asan"
OUT_DIR="$ROOT_DIR/sanitizers"
LOG="$OUT_DIR/asan_ubsan.log"

if [ ! -e "$ROOT_DIR/vendor/karchive/src/karchive.cpp" ]; then
    echo "error: vendor/karchive is empty. Run: git submodule update --init" >&2
    exit 1
fi

echo ">> configuring $BUILD_DIR with ASan + UBSan"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DANALYSIS_SANITIZE=ON >/dev/null

echo ">> building"
cmake --build "$BUILD_DIR" >/dev/null

# detect_leaks=0: Qt keeps intentional global allocations alive for the process
#   lifetime, which LeakSanitizer would report as leaks; they are not ours and
#   not the bug class of interest here (that is memcheck's job -- see
#   ../valgrind).
# halt_on_error=0: keep going after the first finding so one run reports all.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:halt_on_error=0:print_stats=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0"

echo ">> running all suites under sanitizers"
( cd "$BUILD_DIR" && ctest --output-on-failure ) 2>&1 | tee "$LOG"

echo ">> summarising"
{
    echo "AddressSanitizer + UndefinedBehaviorSanitizer run"
    echo "target: KArchive 633dc09, all suites"
    echo
    if grep -qE "runtime error:|ERROR: AddressSanitizer|SUMMARY: (Address|Undefined)" "$LOG"; then
        echo "RESULT: sanitizer findings present -- see asan_ubsan.log"
        grep -E "runtime error:|ERROR: AddressSanitizer|SUMMARY:" "$LOG" | sort | uniq -c
    else
        echo "RESULT: clean -- no ASan or UBSan findings across any suite"
    fi
    echo
    grep -E "tests passed|tests failed" "$LOG" | tail -1
} > "$OUT_DIR/summary.txt"

cat "$OUT_DIR/summary.txt"
