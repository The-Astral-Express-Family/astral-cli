cmake_minimum_required(VERSION 3.25)

project(astral
  VERSION 0.1.1
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

# CRT 链接方式跟随 vcpkg triplet：x64-windows-static 的依赖全是 /MT，
# 本项目对象若留在默认 /MD，链接即 LNK2038 RuntimeLibrary mismatch。
# 判据必须是 VCPKG_TARGET_TRIPLET：VCPKG_CRT_LINKAGE 是 vcpkg 工具侧的
# portfile 变量，toolchain 不导出到消费者 configure 作用域，拿它判断
# 永远不成立（e89a111 首编 CI 实测 LNK2038 的根因）。CMP0091 在
# cmake_minimum_required(3.25) 下为 NEW，此变量在目标创建时生效；
# 非 Windows/vcpkg 构建与动态 triplet 均保持 CMP0091 默认（/MD）。
if(VCPKG_TARGET_TRIPLET MATCHES "-static")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

# Release version embedded into the binary. Release builds pin the tag via
# -DASTRAL_EMBED_VERSION=${GITHUB_REF_NAME} (e.g. "v0.2.0") at configure time.
if(NOT DEFINED ASTRAL_EMBED_VERSION)
  set(ASTRAL_EMBED_VERSION "v${PROJECT_VERSION}")
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
find_package(LibArchive REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# picosha2 的 vcpkg port 只安装裸头文件（无 CMake config），按 header-only 处理。
find_path(PICOSHA2_INCLUDE_DIR picosha2.h REQUIRED)

add_subdirectory(src)

if(ASTRAL_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
