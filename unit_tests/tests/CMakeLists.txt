# Our own tests, built against the pinned KArchive submodule.
#
# Kept in a subdirectory of its own so the analysis code is clearly separated
# from the analysed code in vendor/karchive.

find_package(Qt6 6.8.0 REQUIRED COMPONENTS Core Test)

# KArchive's autotests turn this off, but our code is happy to be explicit
# about QString construction, so leave the default strict setting alone here.

# ---------------------------------------------------------------------------
# Smoke driver
# ---------------------------------------------------------------------------
# A minimal hand-run program that opens an archive and walks its entries.
# It has no assertions and is not part of `ctest`; it exists as a quick
# sanity check that the library links and runs. The real QTest suites are
# added below as they are written.
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
# Each suite is a separate executable registered with CTest, matching the
# convention KArchive uses in autotests/. Added in the tests stage of the
# analysis; see unit_tests/RunningTests.md.
#
# Example of the shape they take:
#
#   add_executable(vs_ktartest vs_ktartest.cpp)
#   target_link_libraries(vs_ktartest PRIVATE KF6::Archive Qt6::Core Qt6::Test)
#   add_test(NAME vs_ktartest COMMAND vs_ktartest)
