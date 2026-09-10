cmake_minimum_required(VERSION 3.20)
project(sb_smoke LANGUAGES C)

# Tiny standalone binary — no engine deps, just libdl. scripts/build.sh builds
# it after the four cores and runs it as the milestone-2 isolation proof.
add_executable(sb_smoke sb_smoke.c)
set_target_properties(sb_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(NOT APPLE)
  target_link_libraries(sb_smoke PRIVATE dl)
endif()
