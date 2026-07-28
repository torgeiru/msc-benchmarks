cmake_minimum_required(VERSION 3.6)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

project(VirtioFS_Whole_File_Read_Benchmark)

if (DEFINED UNIKERNEL)
  find_package(IncludeOS REQUIRED)
  include(os)

  add_compile_definitions(UNIKERNEL)

  set(VIRTIOFS_DRIVER "virtiofs_polling" CACHE STRING
    "VirtioFS driver variant (virtiofs_polling or virtiofs_interrupts)")
  if (NOT VIRTIOFS_DRIVER STREQUAL "virtiofs_polling"
      AND NOT VIRTIOFS_DRIVER STREQUAL "virtiofs_interrupts")
    message(FATAL_ERROR "Invalid VIRTIOFS_DRIVER: ${VIRTIOFS_DRIVER}")
  endif()

  os_add_executable(virtiofs_bench "VirtioFS whole-file read benchmark" main.cpp)
  os_add_stdout(virtiofs_bench default_stdout)
  os_add_drivers(virtiofs_bench ${VIRTIOFS_DRIVER})
else()
  add_executable(virtiofs_bench main.cpp)
  target_link_options(virtiofs_bench PRIVATE -static)
endif()
