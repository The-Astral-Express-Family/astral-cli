add_executable(astral-unit-tests
  test_binding.cpp
  test_cli.cpp
  test_device_flow.cpp
  test_file_credential_store.cpp
  test_json_output.cpp
  test_resolve.cpp
  test_sse.cpp
  test_target.cpp
  test_user_dirs.cpp
)

target_link_libraries(astral-unit-tests PRIVATE astral::core Catch2::Catch2WithMain)
astral_set_warnings(astral-unit-tests)

catch_discover_tests(astral-unit-tests)
