#!/usr/bin/env bash
#
# Runs an AFL++ campaign against one KArchive fuzzer.
#
# Reproduce with:   ./afl/run_campaign.sh <fuzzer> [seconds]
#   e.g.            ./afl/run_campaign.sh k7z 300
# Fuzzers: k7z ktar kzip kar (and the ktar_<codec> device fuzzers)
# Requires: ./afl/build_fuzzers.sh to have been run first.
#
# !!! MEMORY WARNING !!!
# KArchive's 7z parser can be driven to allocate many gigabytes from a tiny
# malformed input (see afl/TRIAGE.md, finding 6). Reproducing a saved hang
# UNCAPPED can exhaust system RAM and take down other applications. This script
# runs the fuzzer under an ASan RSS ceiling (AFL_USE_ASAN builds honour
# hard_rss_limit_mb) and a per-exec timeout, so the campaign itself is bounded.
# When you later reproduce a saved crash/hang by hand, ALWAYS cap it:
#   ASAN_OPTIONS=hard_rss_limit_mb=2048 timeout 10 <fuzzer> afl/hangs/<file>

set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-fuzz"
FUZZER_NAME="${1:-k7z}"
DURATION="${2:-300}"
BIN="$BUILD_DIR/bin/fuzzers/${FUZZER_NAME}_fuzzer"
DICT="$ROOT_DIR/vendor/karchive/autotests/data/dict/${FUZZER_NAME}_fuzzer.dict"
DATA="$ROOT_DIR/vendor/karchive/autotests/data"
OUT="$ROOT_DIR/afl/out_${FUZZER_NAME}"

[ -x "$BIN" ] || { echo "error: $BIN not built. Run ./afl/build_fuzzers.sh" >&2; exit 1; }

# Assemble the seed corpus at runtime from the sample archives already in the
# submodule, so we do not commit copies of files git already tracks via the
# submodule. Any format-specific extra seeds we DO own (e.g. the crafted
# AES-header 7z) live in afl/seeds and are added on top.
SEEDS="$(mktemp -d)"; trap 'rm -rf "$SEEDS"' EXIT
case "$FUZZER_NAME" in
    k7z)  cp "$DATA"/*.7z "$SEEDS"/ 2>/dev/null; cp "$ROOT_DIR/afl/seeds/aes_header.7z" "$SEEDS"/ 2>/dev/null;;
    kzip) cp "$DATA"/*.zip "$SEEDS"/ 2>/dev/null;;
    ktar*) cp "$DATA"/corpus/sample.tar "$DATA"/*.tar.gz "$SEEDS"/ 2>/dev/null;;
    kar)  cp "$DATA"/artest.a "$DATA"/corpus/sample.ar "$SEEDS"/ 2>/dev/null;;
esac
# AFL needs at least one seed.
[ -z "$(ls -A "$SEEDS")" ] && printf '\x00' > "$SEEDS/min"

export LD_LIBRARY_PATH="$BUILD_DIR/bin:${QT_PATH:-$HOME/Qt/6.8.3/gcc_64}/lib:${LD_LIBRARY_PATH:-}"
# core_pattern on this machine is apport; AFL wants a plain core dump path. We
# do not have root here, so tell AFL to proceed anyway -- ASan reports crashes
# via exit status regardless.
export AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES=1
export AFL_SKIP_CPUFREQ=1
export AFL_NO_UI=1
# Bound each execution's memory and time so a discovered amplification bomb
# cannot run the whole machine out of RAM.
export ASAN_OPTIONS="detect_leaks=0:hard_rss_limit_mb=2048:abort_on_error=1:symbolize=0"

DICT_ARG=(); [ -f "$DICT" ] && DICT_ARG=(-x "$DICT")

echo ">> fuzzing $FUZZER_NAME for ${DURATION}s (RSS-capped at 2 GB/exec, 2 s timeout)"
rm -rf "$OUT"
timeout "$DURATION" afl-fuzz \
    -i "$SEEDS" -o "$OUT" \
    "${DICT_ARG[@]}" \
    -m none -t 2000 \
    -- "$BIN" @@ || true

echo
echo ">> results:"
grep -E "execs_done|execs_per_sec|corpus_count|saved_crashes|saved_hangs|bitmap_cvg" "$OUT/default/fuzzer_stats" 2>/dev/null
echo ">> crashes: $(ls "$OUT/default/crashes" 2>/dev/null | grep -c ^id || echo 0), hangs: $(ls "$OUT/default/hangs" 2>/dev/null | grep -c ^id || echo 0)"
