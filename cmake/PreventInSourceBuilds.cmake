# Fail fast when the user configures the build inside the source tree.
get_filename_component(_astral_source_dir "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(_astral_binary_dir "${CMAKE_BINARY_DIR}" REALPATH)
if(_astral_source_dir STREQUAL _astral_binary_dir)
  message(FATAL_ERROR
    " In-source builds are disabled. Use a preset instead:\n"
    "   cmake --preset dev        (debug, vcpkg)\n"
    "   cmake --preset release    (release, vcpkg)\n"
    "Build directories live under ./build/<preset>.")
endif()
