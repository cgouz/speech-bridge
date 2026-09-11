cmake_minimum_required(VERSION 3.20)
project(sb_tts_magpie LANGUAGES C CXX)

# Standalone core: libsb_tts_magpie statically links magpie-tts.cpp + its OWN
# ggml (built with GGML_MAX_NAME=128, set by magpie's own CMakeLists). Separate
# build tree from every other core.

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
option(SB_CUDA  "Build the engine's ggml with the CUDA backend (needs CUDA toolkit)" OFF)

set(SB_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(SB_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/magpie-tts.cpp")
include(${SB_COMMON_DIR}/sb_core.cmake)

set(MAGPIE_BUILD_CLI    OFF CACHE BOOL "" FORCE)
set(MAGPIE_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(MAGPIE_SHARED       OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

if(SB_METAL AND APPLE)
  set(MAGPIE_GGML_METAL       ON CACHE BOOL "" FORCE)
  set(GGML_METAL              ON CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
endif()

if(SB_CUDA)
  if(APPLE)
    message(FATAL_ERROR "SB_CUDA is not supported on Apple hardware; use SB_METAL instead")
  endif()
  # magpie-tts.cpp forwards this to its own vendored ggml's GGML_CUDA (see
  # third_party/magpie-tts.cpp/CMakeLists.txt); its Backend class auto-selects
  # a non-CPU ggml device at runtime the same way parakeet.cpp's does.
  set(MAGPIE_GGML_CUDA ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${SB_ENGINE_DIR} "${CMAKE_BINARY_DIR}/engine" EXCLUDE_FROM_ALL)

add_library(sb_tts_magpie SHARED sb_tts_magpie.cpp)
target_compile_options(sb_tts_magpie PRIVATE -fvisibility=hidden -Wall -Wextra)
target_include_directories(sb_tts_magpie PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SB_COMMON_DIR}
  ${SB_ENGINE_DIR}/include)
target_link_libraries(sb_tts_magpie PRIVATE magpie-tts)

set_target_properties(sb_tts_magpie PROPERTIES OUTPUT_NAME sb_tts_magpie PREFIX "lib")
sb_core_isolate(sb_tts_magpie)

install(TARGETS sb_tts_magpie LIBRARY DESTINATION lib RUNTIME DESTINATION lib)
