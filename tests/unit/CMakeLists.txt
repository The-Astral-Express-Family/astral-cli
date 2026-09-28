add_executable(astral-unit-tests
  test_archive.cpp
  test_binding.cpp
  test_cli.cpp
  test_content_hash.cpp
  test_device_flow.cpp
  test_distribution.cpp
  test_document_cmd.cpp
  test_error.cpp
  test_event_cmd.cpp
  test_file_credential_store.cpp
  test_github_release.cpp
  test_http_options.cpp
  test_init_cmd.cpp
  test_json_output.cpp
  test_profile_cmd.cpp
  test_register_cmd.cpp
  test_resolve.cpp
  test_self_update.cpp
  test_semver.cpp
  test_sse.cpp
  test_tags_msg_cmd.cpp
  test_target.cpp
  test_todo_cmd.cpp
  test_update_cmd.cpp
  test_user_dirs.cpp
)

target_link_libraries(astral-unit-tests PRIVATE astral::core Catch2::Catch2WithMain)
astral_set_warnings(astral-unit-tests)

catch_discover_tests(astral-unit-tests)
