cmake_minimum_required(VERSION 3.25)

project(astral
  VERSION 0.1.0
  DESCRIPTION "Astral CLI - client for Astral servers"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# Keep every executable at the root of the preset's build directory so
# scripts and docs can reference a single stable path (build/<preset>/astral).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

if(MSVC)
  add_compile_options(/utf-8)
endif()

include(cmake/PreventInSourceBuilds.cmake)
include(cmake/StaticAnalyzers.cmake)
include(cmake/Warnings.cmake)

option(ASTRAL_BUILD_TESTS "Build unit tests" ON)
option(ASTRAL_ENABLE_INTEGRATION_TESTS "Build tests that need a live server" OFF)
option(ASTRAL_ENABLE_CONTRACT_TESTS "Build protocol snapshot contract tests" OFF)
option(ASTRAL_ENABLE_SANITIZERS "Build with ASan+UBSan (gcc/clang only)" OFF)

if(ASTRAL_ENABLE_SANITIZERS AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Capture the git revision at configure time for `astral version`.
find_package(Git QUIET)
set(ASTRAL_GIT_DESCRIBE "unknown")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE ASTRAL_GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

find_package(CLI11 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_subdirectory(src)

if(ASTRAL_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
