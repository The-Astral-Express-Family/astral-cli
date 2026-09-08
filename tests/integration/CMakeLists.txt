# Integration tests hit a real server via libcurl. They are OFF by default
# and need ASTRAL_TEST_SERVER to point at a reachable Astral server
# (e.g. http://localhost:8080 for local development).

add_executable(astral-integration-tests test_http_client.cpp)
target_link_libraries(astral-integration-tests PRIVATE astral::core Catch2::Catch2WithMain)
astral_set_warnings(astral-integration-tests)

catch_discover_tests(astral-integration-tests)
