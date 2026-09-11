set(CATCH2_DIR "${THIRD_PARTY_DIR}/catch2")

add_library(catch2_amalgamated STATIC "${CATCH2_DIR}/src/catch_amalgamated.cpp")
target_include_directories(catch2_amalgamated PUBLIC "${CATCH2_DIR}/src")

add_executable(sb_tests
  text_test.cpp
  config_test.cpp
  wav_test.cpp
)
target_link_libraries(sb_tests PRIVATE sb_lib catch2_amalgamated)

include(CTest)
add_test(NAME sb_tests COMMAND sb_tests)
