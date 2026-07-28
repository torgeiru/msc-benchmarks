cmake_minimum_required(VERSION 3.6)
# set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(VirtioFS_Benchmark)

set(CMAKE_CXX_STANDARD "20")
# set(CMAKE_CXX_COMPILER "clang++")

add_executable(virtiofs_bench main.cpp)
target_link_libraries(virtiofs_bench PRIVATE uring)