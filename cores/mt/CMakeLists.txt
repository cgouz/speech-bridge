cmake_minimum_required(VERSION 3.20)
project(sb_mt LANGUAGES C CXX)

# Standalone core: libsb_mt statically links llama.cpp + its OWN in-tree ggml.
# Separate build tree from every other core.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()

option(SB_METAL "Build llama.cpp's ggml with the Metal backend (needs full Xcode)" OFF)
option(SB_CUDA  "Build llama.cpp's ggml with the CUDA backend (needs CUDA toolkit)" OFF)

set(SB_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(SB_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/llama.cpp")
include(${SB_COMMON_DIR}/sb_core.cmake)

set(LLAMA_BUILD_IS_DEV   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP      OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)

if(SB_METAL AND APPLE)
  set(GGML_METAL               ON CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
else()
  set(GGML_METAL OFF CACHE BOOL "" FORCE)
endif()

if(SB_CUDA)
  if(APPLE)
    message(FATAL_ERROR "SB_CUDA is not supported on Apple hardware; use SB_METAL instead")
  endif()
  set(GGML_CUDA ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${SB_ENGINE_DIR} "${CMAKE_BINARY_DIR}/engine" EXCLUDE_FROM_ALL)

add_library(sb_mt SHARED sb_mt.cpp)
target_compile_options(sb_mt PRIVATE -fvisibility=hidden -Wall -Wextra)
target_include_directories(sb_mt PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SB_COMMON_DIR}
  ${SB_ENGINE_DIR}/include)
target_link_libraries(sb_mt PRIVATE llama)

# llama.cpp forces CMAKE_LIBRARY_OUTPUT_DIRECTORY to <build>/bin globally; put
# our core lib back at the build root so scripts/build.sh finds it.
set_target_properties(sb_mt PROPERTIES
  OUTPUT_NAME sb_mt
  PREFIX "lib"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
sb_core_isolate(sb_mt)

install(TARGETS sb_mt LIBRARY DESTINATION lib RUNTIME DESTINATION lib)
