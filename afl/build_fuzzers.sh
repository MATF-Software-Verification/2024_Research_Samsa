#!/usr/bin/env bash
#
# Builds KArchive's OSS-Fuzz harnesses under AFL++ instrumentation with ASan.
#
# Reproduce with:   ./afl/build_fuzzers.sh
# Requires:         afl++ (afl-clang-fast), clang, cmake, ninja,
#                   Qt >= 6.8, extra-cmake-modules >= 6.21
#
# KArchive already ships libFuzzer harnesses in autotests/ossfuzz (it is in
# Google's OSS-Fuzz). We reuse them unchanged: their CMakeLists honours a
# LIB_FUZZING_ENGINE env var, so we point it at AFL++'s libAFLDriver.a, and the
# whole fuzz build is then compiled by afl-clang-fast. Nothing is patched.
#
# Why AFL++ rather than the libFuzzer the harnesses were written for:
#   - libFuzzer was covered in the course exercises; AFL++ was not.
#   - AFL++ has a hang/timeout detector, which directly targets the
#     malformed-input DoS class that KArchive's own history is full of
#     ("7z: Fix infinite loop in malformed file").
#   - It drives the existing LLVMFuzzerTestOneInput harnesses via libAFLDriver,
#     so we reuse upstream's harness rather than writing an inferior one.
#
# The k7z fuzzer is built with -DUSE_PASSWORD=1 by upstream when OpenSSL is
# found, which sets a dummy password and so reaches the AES-decryption path.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-fuzz"

if [ ! -e "$ROOT_DIR/vendor/karchive/src/karchive.cpp" ]; then
    echo "error: vendor/karchive is empty. Run: git submodule update --init" >&2
    exit 1
fi
command -v afl-clang-fast >/dev/null || { echo "error: afl++ not installed" >&2; exit 1; }

export AFL_USE_ASAN=1        # crashes that only ASan sees still count
export AFL_QUIET=1
export LIB_FUZZING_ENGINE=/usr/lib/afl/libAFLDriver.a

echo ">> configuring $BUILD_DIR (afl-clang-fast + ASan)"
CC=afl-clang-fast CXX=afl-clang-fast++ \
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
          -DBUILD_FUZZERS=ON -DBUILD_TESTING=OFF >/dev/null

echo ">> building fuzzers"
cmake --build "$BUILD_DIR" >/dev/null

echo ">> fuzzers built in $BUILD_DIR/bin/fuzzers:"
ls "$BUILD_DIR/bin/fuzzers"
