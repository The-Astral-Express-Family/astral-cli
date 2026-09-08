find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_subdirectory(unit)

if(ASTRAL_ENABLE_INTEGRATION_TESTS)
  add_subdirectory(integration)
endif()

if(ASTRAL_ENABLE_CONTRACT_TESTS)
  add_subdirectory(contract)
endif()
