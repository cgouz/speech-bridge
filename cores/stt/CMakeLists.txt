cmake_minimum_required(VERSION 3.20)
project(sb_stt LANGUAGES C CXX)

# Standalone CMake project: one build tree per core so the engines' mutually
# incompatible ggml copies never collide on a target name or a link unit.
# scripts/build.sh configures and builds this into build/stt/ and installs
# libsb_stt.(dylib|so) into lib/.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()

option(SB_METAL "Build the engine's ggml with the Metal backend (needs full Xcode)" OFF)

set(SB_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(SB_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/parakeet.cpp")
include(${SB_COMMON_DIR}/sb_core.cmake)

# --- vendored engine: parakeet.cpp (+ its own patched ggml) ----------------
set(PARAKEET_BUILD_CLI     OFF CACHE BOOL "" FORCE)
set(PARAKEET_BUILD_SERVER  OFF CACHE BOOL "" FORCE)
set(PARAKEET_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(PARAKEET_SHARED        OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)  # ggml must be static, inside libsb_stt

if(SB_METAL AND APPLE)
  set(PARAKEET_GGML_METAL     ON  CACHE BOOL "" FORCE)
  set(GGML_METAL              ON  CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${SB_ENGINE_DIR} "${CMAKE_BINARY_DIR}/engine" EXCLUDE_FROM_ALL)

# parakeet.cpp's CMakeLists.txt uses ${CMAKE_SOURCE_DIR} (line 107) to locate its
# bundled third_party/ (dr_wav.h). That variable points at THIS project when the
# tree is vendored as a subdirectory, so the path resolves wrong. We cannot edit
# third_party/, so append the correct include dir to the engine target here.
# (Its scripts/apply_ggml_patches.sh is likewise skipped for the same reason —
#  harmless: the four ggml patches are pre-applied in the vendored tree. See
#  docs/blockers.md.)
target_include_directories(parakeet PRIVATE "${SB_ENGINE_DIR}/third_party")

# --- the core shared library ----------------------------------------------
add_library(sb_stt SHARED sb_stt.cpp)
target_compile_options(sb_stt PRIVATE -fvisibility=hidden -Wall -Wextra)
target_include_directories(sb_stt PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SB_COMMON_DIR}
  ${SB_ENGINE_DIR}/include)
target_link_libraries(sb_stt PRIVATE parakeet)

set_target_properties(sb_stt PROPERTIES
  OUTPUT_NAME sb_stt
  PREFIX "lib")
sb_core_isolate(sb_stt)

install(TARGETS sb_stt LIBRARY DESTINATION lib RUNTIME DESTINATION lib)
