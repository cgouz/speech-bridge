cmake_minimum_required(VERSION 3.20)
project(sb_tts_vits LANGUAGES C CXX)

# Standalone core: libsb_tts_vits statically links sherpa-onnx (C API + core)
# and onnxruntime. No ggml — pure C++/onnxruntime, so it cannot collide with
# the ggml cores; it still builds in its own tree and exports only sb_*.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()

option(SB_METAL "unused for this core" OFF)

set(SB_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")
set(SB_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sherpa-onnx")
include(${SB_COMMON_DIR}/sb_core.cmake)

# Trim sherpa-onnx to just the offline-TTS C API.
set(SHERPA_ONNX_ENABLE_PYTHON              OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TESTS               OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_CHECK               OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_PORTAUDIO           OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_JNI                 OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_C_API              ON  CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_WEBSOCKET           OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_GPU                 OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_BINARY             OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_TTS                ON  CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_BUILD_C_API_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(SHERPA_ONNX_ENABLE_WASM                OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS                      OFF CACHE BOOL "" FORCE)

add_subdirectory(${SB_ENGINE_DIR} "${CMAKE_BINARY_DIR}/engine" EXCLUDE_FROM_ALL)

add_library(sb_tts_vits SHARED sb_tts_vits.cpp)
target_compile_options(sb_tts_vits PRIVATE -fvisibility=hidden -Wall -Wextra)
target_include_directories(sb_tts_vits PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${SB_COMMON_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../tts_magpie   # shared sb_tts.h ABI header
  ${SB_ENGINE_DIR})
target_link_libraries(sb_tts_vits PRIVATE sherpa-onnx-c-api)

set_target_properties(sb_tts_vits PROPERTIES OUTPUT_NAME sb_tts_vits PREFIX "lib")
sb_core_isolate(sb_tts_vits)

# If sherpa produced a shared onnxruntime, copy it next to the core lib so the
# @loader_path / $ORIGIN rpath resolves it.
get_target_property(_ort_libs sherpa-onnx-c-api INTERFACE_LINK_LIBRARIES)
install(TARGETS sb_tts_vits LIBRARY DESTINATION lib RUNTIME DESTINATION lib)
