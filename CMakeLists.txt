cmake_minimum_required(VERSION 3.16)
project(2024_Research_Samsa)

find_package(KF6Archive REQUIRED)
find_package(Qt6Core REQUIRED)

add_executable(test_karchive test_karchive.cpp)
target_link_libraries(test_karchive KF6::Archive Qt6::Core)
