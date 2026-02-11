cmake_minimum_required(VERSION 3.16)

# KArchive requires CMake version 6.8.0 or higher
set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.8.3/gcc_64" ${CMAKE_PREFIX_PATH})

project(KArchiveAnalysis VERSION 1.0.0 LANGUAGES CXX)

# KArchive requires C++20 or higher
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8.3 REQUIRED COMPONENTS Core)
find_package(ZLIB REQUIRED)
find_package(LibLZMA REQUIRED)

# Includes the KArchive submodule
add_subdirectory(vendor/karchive)

add_executable(karchive_verify unit_tests/tests/main.cpp)

target_link_libraries(karchive_verify
    PRIVATE
        KF6Archive
        Qt6::Core
)

set_target_properties(karchive_verify PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)