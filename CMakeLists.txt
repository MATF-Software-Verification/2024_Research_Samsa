cmake_minimum_required(VERSION 3.16)

# KArchive requires CMake version 6.8.0 or higher
set(CMAKE_PREFIX_PATH "$ENV{HOME}/Qt/6.8.3/gcc_64" ${CMAKE_PREFIX_PATH})

project(KArchiveAnalysis VERSION 1.0.0 LANGUAGES CXX)

# KArchive requires C++20 or higher
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8.3 REQUIRED COMPONENTS Core)

# Includes the KArchive submodule
add_subdirectory(vendor/karchive)