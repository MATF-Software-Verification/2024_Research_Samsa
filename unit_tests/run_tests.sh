#!/usr/bin/env bash
#
# Builds and runs every test suite under gcov instrumentation, then produces
# both a baseline (KArchive's own autotests only) and a combined measurement,
# so the contribution of our suites is visible as a delta rather than asserted.
#
# Reproduce with:   ./unit_tests/run_tests.sh
# Requires:         cmake, ninja, gcovr, gcov, Qt >= 6.8,
#                   extra-cmake-modules >= 6.21
#
# Outputs (all committed):
#   unit_tests/coverage/baseline.txt    upstream autotests only
#   unit_tests/coverage/combined.txt    upstream + ours
#   unit_tests/coverage/delta.txt       per-file branch coverage difference
#   unit_tests/coverage/coverage.html   browsable combined report
#   unit_tests/coverage/ctest.log       the test run itself
#
# NOTE on tooling: `lcov --list` is broken in lcov 2.0-1 as shipped by Ubuntu
# 24.04 -- it reports ~11% line coverage and 0% function coverage for data that
# genhtml and gcovr both read as ~82%/89%. The .info file itself is correct, so
# the fault is in --list's summary only. We use gcovr for the numbers to avoid
# depending on that. See unit_tests/RunningTests.md.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-coverage"
OUT_DIR="$ROOT_DIR/unit_tests/coverage"
FILTER='vendor/karchive/src/'

if [ ! -e "$ROOT_DIR/vendor/karchive/src/karchive.cpp" ]; then
    echo "error: vendor/karchive is empty. Run: git submodule update --init" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

echo ">> configuring $BUILD_DIR with coverage instrumentation"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja -DANALYSIS_COVERAGE=ON >/dev/null

echo ">> building"
cmake --build "$BUILD_DIR" >/dev/null

# gcov accumulates counts across runs, so counters must be cleared between the
# two measurements or the baseline would include our suites.
clear_counters() {
    find "$BUILD_DIR" -name '*.gcda' -delete
}

measure() {
    local label="$1" outfile="$2"
    echo ">> measuring: $label"
    gcovr --root "$ROOT_DIR" --filter "$FILTER" --txt "$outfile" --print-summary 2>/dev/null | tail -3
}

# ---------------------------------------------------------------------------
# 1. Baseline -- KArchive's own autotests, excluding everything we wrote.
# ---------------------------------------------------------------------------
clear_counters
echo ">> running upstream autotests only"
( cd "$BUILD_DIR" && ctest --exclude-regex '^vs_' --output-on-failure ) > "$OUT_DIR/ctest-baseline.log" 2>&1 || true
tail -3 "$OUT_DIR/ctest-baseline.log"
measure "baseline (upstream autotests only)" "$OUT_DIR/baseline.txt"
gcovr --root "$ROOT_DIR" --filter "$FILTER" --txt-metric branch --txt "$OUT_DIR/baseline-branch.txt" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# 2. Combined -- upstream autotests plus ours.
# ---------------------------------------------------------------------------
clear_counters
echo ">> running all suites"
( cd "$BUILD_DIR" && ctest --output-on-failure ) > "$OUT_DIR/ctest.log" 2>&1 || true
tail -3 "$OUT_DIR/ctest.log"
measure "combined (upstream + ours)" "$OUT_DIR/combined.txt"
gcovr --root "$ROOT_DIR" --filter "$FILTER" --txt-metric branch --txt "$OUT_DIR/combined-branch.txt" >/dev/null 2>&1

echo ">> writing browsable report"
# A single self-contained page. --html-details would add a per-file page with
# annotated source, which is genuinely useful but produces ~5 MB of HTML that
# would be re-committed on every run; the per-file numbers are already in the
# .txt reports. Add --html-details locally when you want the annotated source.
gcovr --root "$ROOT_DIR" --filter "$FILTER" \
      --html --html-self-contained \
      --output "$OUT_DIR/coverage.html" >/dev/null 2>&1

# ---------------------------------------------------------------------------
# 3. The delta, which is the actual result of this stage.
# ---------------------------------------------------------------------------
echo ">> computing delta"
python3 - "$OUT_DIR" <<'PY'
import re, sys, pathlib

out = pathlib.Path(sys.argv[1])

def parse(path):
    rows = {}
    for line in path.read_text().splitlines():
        m = re.match(r'(vendor/karchive/src/\S+)\s+(\d+)\s+(\d+)\s+(\d+)%', line)
        if m:
            rows[m.group(1)] = int(m.group(4))
    return rows

lines = []
for metric, base_f, comb_f in (("branch", "baseline-branch.txt", "combined-branch.txt"),
                               ("line", "baseline.txt", "combined.txt")):
    base, comb = parse(out / base_f), parse(out / comb_f)
    lines.append(f"=== {metric} coverage, per file ===")
    lines.append(f"{'file':<32}{'baseline':>10}{'combined':>10}{'delta':>9}")
    changed = []
    for f in sorted(set(base) | set(comb)):
        b, c = base.get(f, 0), comb.get(f, 0)
        changed.append((c - b, f.replace("vendor/karchive/src/", ""), b, c))
    for d, f, b, c in sorted(changed, key=lambda r: -r[0]):
        mark = f"{d:+d}pp" if d else "-"
        lines.append(f"{f:<32}{b:>9}%{c:>9}%{mark:>9}")
    lines.append("")

(out / "delta.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines[:14]))
PY

echo ">> done: reports in unit_tests/coverage/"
