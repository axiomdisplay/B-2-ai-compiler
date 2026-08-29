cmake_minimum_required(VERSION 3.24)

project(B2
  VERSION 0.1.0
  DESCRIPTION "B-2 - AI-built Java runtime and tiered compiler"
  LANGUAGES CXX)

# The whole project is C++26.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)

option(B2_BUILD_TOOLS "Build command-line tools (b2parse)" ON)
option(B2_BUILD_TESTS "Build the test suite" ON)

enable_testing()

add_subdirectory(compiler/frontend)

if(B2_BUILD_TESTS)
  add_subdirectory(tests/frontend)
endif()
