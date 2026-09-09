#!/usr/bin/env bash
#
# Runs cppcheck over KArchive's library sources.
#
# Reproduce with:   ./cppcheck/run_cppcheck.sh
# Requires:         cppcheck >= 2.13, cmake, ninja, python3,
#                   Qt >= 6.8, extra-cmake-modules >= 6.21
#
# Outputs (all committed):
#   cppcheck/cppcheck_report.xml   machine-readable, one <error> per finding
#   cppcheck/cppcheck_report.txt   human-readable, with the offending line
#   cppcheck/summary.txt           finding counts per check id and severity
#
# cppcheck is the second, independent static engine in this analysis. Unlike
# clang-tidy it does not use a real compiler frontend; it runs its own
# value-flow analysis over possible execution paths, which is why it reports a
# different class of defect. Comparing the two is part of the result -- see
# cppcheck/TRIAGE.md.
#
# The analysed submodule is never modified.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-analysis"
OUT_DIR="$ROOT_DIR/cppcheck"
CC_DB="$BUILD_DIR/compile_commands.json"
CC_FILTERED="$BUILD_DIR/compile_commands.karchive-src.json"

if [ ! -e "$ROOT_DIR/vendor/karchive/src/karchive.cpp" ]; then
    echo "error: vendor/karchive is empty. Run: git submodule update --init" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. A build is needed only for compile_commands.json and the generated
#    headers the sources include (karchive_export.h, karchive_version.h).
# ---------------------------------------------------------------------------
echo ">> configuring $BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DBUILD_TESTING=OFF >/dev/null

echo ">> generating headers"
cmake --build "$BUILD_DIR" --target KF6Archive_autogen >/dev/null

# ---------------------------------------------------------------------------
# 2. Restrict the compile database to KArchive's own sources.
#    Dropped: moc/autogen output and ECMQmLoader (both generated, not written
#    by the KArchive authors, so findings in them are not about this project).
# ---------------------------------------------------------------------------
echo ">> filtering compile database"
python3 - "$CC_DB" "$CC_FILTERED" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
entries = json.load(open(src))
keep = [
    e for e in entries
    if "/vendor/karchive/src/" in e["file"]
    and "_autogen" not in e["file"]
    and "/build-" not in e["file"]
    and "ECMQmLoader" not in e["file"]
]
json.dump(keep, open(dst, "w"), indent=1)
print(f"   {len(keep)} of {len(entries)} translation units kept")
PY

# ---------------------------------------------------------------------------
# 3. Analyse.
# ---------------------------------------------------------------------------
# --library=qt          teaches cppcheck Qt's ownership and container semantics,
#                       which removes a large class of false positives
# --check-level=exhaustive  deeper value-flow analysis (slower, more findings)
# --inconclusive        report findings cppcheck cannot fully prove; these are
#                       marked inconclusive="true" in the XML and are triaged
#                       separately rather than trusted blindly
# unusedFunction is suppressed: KArchive is a library, so its public API is
# unused *within* the project by definition. missingIncludeSystem likewise
# only reports that system headers were not parsed, which is expected.
COMMON_ARGS=(
    --project="$CC_FILTERED"
    --enable=warning,style,performance,portability
    --inconclusive
    --check-level=exhaustive
    --library=qt
    --std=c++20
    --suppress=missingIncludeSystem
    --suppress=unusedFunction
    --suppress=checkersReport
)

echo ">> running cppcheck (XML)"
cppcheck "${COMMON_ARGS[@]}" --xml --output-file="$OUT_DIR/cppcheck_report.xml" 2>/dev/null

echo ">> running cppcheck (text)"
cppcheck "${COMMON_ARGS[@]}" \
    --template='{file}:{line}:{column}: {severity}: {message} [{id}]' \
    --output-file="$OUT_DIR/cppcheck_report.txt" 2>/dev/null

# Paths are absolute and machine-specific; make them repository-relative so the
# committed reports are stable across machines.
sed -i "s|$ROOT_DIR/||g" "$OUT_DIR/cppcheck_report.txt" "$OUT_DIR/cppcheck_report.xml"

# ---------------------------------------------------------------------------
# 4. Summarise.
# ---------------------------------------------------------------------------
echo ">> summarising"
python3 - "$OUT_DIR/cppcheck_report.xml" "$OUT_DIR/summary.txt" <<'PY'
import sys, collections, xml.etree.ElementTree as ET
xml_path, out_path = sys.argv[1], sys.argv[2]
root = ET.parse(xml_path).getroot()
errors = root.findall(".//error")

by_sev = collections.Counter(e.get("severity") for e in errors)
by_id  = collections.Counter(e.get("id") for e in errors)
incon  = sum(1 for e in errors if e.get("inconclusive") == "true")
by_file = collections.Counter(
    loc.get("file") for e in errors for loc in e.findall("location")[:1]
)

with open(out_path, "w") as f:
    f.write(f"cppcheck findings: {len(errors)} total ({incon} inconclusive)\n\n")
    f.write("By severity\n")
    for sev, n in by_sev.most_common():
        f.write(f"  {n:5d}  {sev}\n")
    f.write("\nBy check id\n")
    for cid, n in by_id.most_common():
        f.write(f"  {n:5d}  {cid}\n")
    f.write("\nBy file\n")
    for fn, n in by_file.most_common():
        f.write(f"  {n:5d}  {fn}\n")
print(open(out_path).read())
PY

echo ">> done: cppcheck/cppcheck_report.{xml,txt} and cppcheck/summary.txt"
