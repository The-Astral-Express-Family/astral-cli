# Contract tests pin the CLI against a frozen protocol snapshot in
# protocol/snapshots/ (see protocol/README.md). Enable with
# -DASTRAL_ENABLE_CONTRACT_TESTS=ON; CI runs them against the snapshot
# version this CLI targets, not against a live server.

add_executable(astral-contract-tests
  test_protocol_snapshot.cpp
)
target_link_libraries(astral-contract-tests PRIVATE astral::core Catch2::Catch2WithMain)
target_compile_definitions(astral-contract-tests PRIVATE
  ASTRAL_PROTOCOL_DIR="${CMAKE_SOURCE_DIR}/protocol"
)
astral_set_warnings(astral-contract-tests)

catch_discover_tests(astral-contract-tests)
