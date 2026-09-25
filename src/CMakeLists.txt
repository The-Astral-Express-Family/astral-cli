configure_file(core/version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/core/version.hpp" @ONLY)

add_library(astral_core STATIC
  app/app.cpp
  auth/api.cpp
  auth/device_flow.cpp
  auth/session.cpp
  client/http_client.cpp
  client/sse.cpp
  commands/doctor_cmd.cpp
  commands/document_cmd.cpp
  commands/event_cmd.cpp
  commands/init_cmd.cpp
  commands/login_cmd.cpp
  commands/msg_cmd.cpp
  commands/profile_cmd.cpp
  commands/register_cmd.cpp
  commands/registry.cpp
  commands/tags_cmd.cpp
  commands/todo_cmd.cpp
  commands/version_cmd.cpp
  core/content_hash.cpp
  core/env.cpp
  core/error.cpp
  core/log.cpp
  core/version.cpp
  events/listen.cpp
  output/json_output.cpp
  output/render.cpp
  output/style.cpp
  output/tty.cpp
  platform/args.cpp
  platform/atomic_file.cpp
  platform/browser.cpp
  platform/credential_store.cpp
  platform/stdin.cpp
  platform/user_dirs.cpp
  workspace/binding.cpp
  workspace/resolve.cpp
  workspace/target.cpp
)
add_library(astral::core ALIAS astral_core)

target_compile_features(astral_core PUBLIC cxx_std_20)
target_include_directories(astral_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(astral_core PUBLIC
  CLI11::CLI11
  CURL::libcurl
  fmt::fmt
  nlohmann_json::nlohmann_json
  spdlog::spdlog
)
astral_set_warnings(astral_core)

add_executable(astral app/main.cpp)
target_link_libraries(astral PRIVATE astral::core)
astral_set_warnings(astral)

install(TARGETS astral RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
