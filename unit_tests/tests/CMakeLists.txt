# Our own tests, built against the pinned KArchive submodule.
#
# Kept in a subdirectory of its own so the analysis code is clearly separated
# from the analysed code in vendor/karchive.

find_package(Qt6 6.8.0 REQUIRED COMPONENTS Core Test)

# The suites reuse the sample archives that ship with KArchive's own autotests
# rather than committing binaries of our own -- the CI gate rejects any ELF in
# the repository, and these files are already present via the submodule.
set(KARCHIVE_TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/vendor/karchive/autotests/data")

# ---------------------------------------------------------------------------
# Smoke driver
# ---------------------------------------------------------------------------
# A minimal hand-run program that opens an archive and walks its entries. It
# has no assertions and is deliberately not registered with CTest; it exists
# as a quick check that the library links and runs.
add_executable(karchive_smoke main.cpp)

target_link_libraries(karchive_smoke
    PRIVATE
        KF6::Archive
        Qt6::Core
)

set_target_properties(karchive_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# QTest suites
# ---------------------------------------------------------------------------
# vs_robustnesstest   parsers vs. truncated, corrupted and degenerate input
# vs_roundtriptest    writers and compression filters vs. awkward-but-valid input
foreach(_suite vs_robustnesstest vs_roundtriptest)
    add_executable(${_suite} ${_suite}.cpp)

    target_link_libraries(${_suite}
        PRIVATE
            KF6::Archive
            Qt6::Core
            Qt6::Test
    )

    target_compile_definitions(${_suite}
        PRIVATE
            KARCHIVE_TEST_DATA_DIR="${KARCHIVE_TEST_DATA_DIR}"
    )

    set_target_properties(${_suite} PROPERTIES
        AUTOMOC ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_test(NAME ${_suite} COMMAND ${_suite})

    # The malformed-input suite runs thousands of parse attempts; give it room
    # under valgrind, where everything is 20-50x slower.
    set_tests_properties(${_suite} PROPERTIES TIMEOUT 900)
endforeach()
