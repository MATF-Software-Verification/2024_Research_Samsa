cmake_minimum_required(VERSION 3.16)

project(KArchiveAnalysis VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# What this file is
# ---------------------------------------------------------------------------
# A thin wrapper that builds the pinned KArchive submodule together with our
# own tests, so that every analysis tool has one reproducible build to run on.
#
# The submodule is built via add_subdirectory() and is NEVER modified: compiler
# instrumentation is injected from here instead, so the analysed source stays
# byte-identical to upstream commit 633dc09.
#
# Build variants (see unit_tests/RunningTests.md):
#   plain        cmake -S . -B build
#   coverage     cmake -S . -B build-coverage  -DANALYSIS_COVERAGE=ON
#   sanitizers   cmake -S . -B build-asan      -DANALYSIS_SANITIZE=ON
#   fuzzers      cmake -S . -B build-fuzz      -DBUILD_FUZZERS=ON
#
# Prerequisites that are NOT satisfied by stock Ubuntu 24.04:
#   * extra-cmake-modules >= 6.21.0 -- Ubuntu ships 5.115.0, so a newer ECM
#     must be installed from source. It lands in /usr/local/share/ECM, which
#     CMake searches by default.
#   * Qt >= 6.8.0 with the Core, Test and Network modules.

# ---------------------------------------------------------------------------
# Analysis options
# ---------------------------------------------------------------------------
option(ANALYSIS_COVERAGE "Instrument the build for gcov/lcov coverage" OFF)
option(ANALYSIS_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# clang-tidy and several other tools need compile_commands.json.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Emit compile_commands.json")

# Upstream's own autotests are wanted by default: they provide the baseline
# coverage measurement that the rest of the analysis is aimed by.
set(BUILD_TESTING ON CACHE BOOL "Build the test suites")

# ---------------------------------------------------------------------------
# Qt discovery
# ---------------------------------------------------------------------------
# A caller-supplied CMAKE_PREFIX_PATH always wins; otherwise fall back to a
# Qt installed under $HOME/Qt, which is how this machine is set up. Listing
# candidates rather than hardcoding one keeps the build working when Qt is
# upgraded or when it comes from the distribution instead.
if(NOT CMAKE_PREFIX_PATH)
    foreach(_qt_candidate "$ENV{HOME}/Qt/6.8.3/gcc_64" "$ENV{HOME}/Qt/6.10.1/gcc_64")
        if(EXISTS "${_qt_candidate}")
            list(APPEND CMAKE_PREFIX_PATH "${_qt_candidate}")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Instrumentation
# ---------------------------------------------------------------------------
# Applied before add_subdirectory() so the flags reach KArchive's own sources.
# This is what lets us instrument upstream code without patching it.
if(ANALYSIS_COVERAGE)
    add_compile_options(--coverage -O0 -g -fprofile-abs-path)
    add_link_options(--coverage)
    message(STATUS "KArchiveAnalysis: gcov coverage instrumentation ENABLED")
endif()

if(ANALYSIS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "KArchiveAnalysis: ASan + UBSan ENABLED")
endif()

if(ANALYSIS_COVERAGE AND ANALYSIS_SANITIZE)
    message(WARNING
        "Coverage and sanitizers are enabled together. This works but slows the "
        "build down and muddles both results -- prefer separate build directories.")
endif()

# ---------------------------------------------------------------------------
# The analysed project, then our own tests
# ---------------------------------------------------------------------------
enable_testing()

add_subdirectory(vendor/karchive)
add_subdirectory(unit_tests/tests)
