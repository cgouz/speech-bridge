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

# sherpa-onnx defaults to statically linking libstdc++ into its (otherwise
# static) sub-libraries on Linux. Baked into our SHARED sb_tts_vits.so, that
# leaves two independent libstdc++ runtimes (this one, plus the process's
# normal dynamic libstdc++ from every other core / the Go binary) sharing one
# heap — observed corrupting memory ("free(): invalid pointer") while loading
# a VITS model. Use the dynamic libstdc++ like every other core instead. See
# docs/blockers.md #11.
set(SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY  OFF CACHE BOOL "" FORCE)

# --- onnxruntime version override (Linux x86_64 only) -------------------
# sherpa-onnx's vendored cmake/onnxruntime-linux-x86_64-static.cmake hardcodes
# onnxruntime 1.27.1. That release's Environment::Initialize() unconditionally
# runs a new "device auto-discovery" step
# (onnxruntime::DeviceDiscovery::DiscoverDevicesForPlatform, added ~1.20+)
# that corrupts the heap on first OrtEnv creation ("free(): invalid pointer",
# SIGABRT) — before any of our code runs, so nothing in sb_tts_vits.cpp can
# work around it. Confirmed upstream bug class (also reported on Jetson/RK3588
# hardware: microsoft/onnxruntime#28301, #26763). See docs/blockers.md #11.
#
# Fix: pin onnxruntime 1.18.1 instead, which predates that feature entirely
# (verified: no DeviceDiscovery symbol in the archive). We fetch it ourselves
# — sha256-verified, same discipline as scripts/fetch-models.sh — into the
# build tree, then point CMake's own FETCHCONTENT_SOURCE_DIR_<NAME> override
# at it. That is FetchContent's sanctioned mechanism for a parent project to
# replace a named dependency's source without editing the file that declares
# it: when set, the vendored FetchContent_Declare(onnxruntime URL ... HASH
# ...) call's own URL/hash is never consulted for that dependency.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  set(_sb_ort_url  "https://github.com/csukuangfj/onnxruntime-libs/releases/download/v1.18.1/onnxruntime-linux-x64-static_lib-1.18.1-glibc2_17.zip")
  set(_sb_ort_hash "SHA256=48e0691efa6b043f47f4d53e81cdcc151077c8ae4fab049fb00844ec2d61632a")
  set(_sb_ort_dir  "${CMAKE_BINARY_DIR}/_ort_override")
  set(_sb_ort_zip  "${CMAKE_BINARY_DIR}/_ort_override.zip")
  if(NOT EXISTS "${_sb_ort_dir}")
    message(STATUS "Fetching onnxruntime 1.18.1 (Linux x86_64 DeviceDiscovery workaround)")
    file(DOWNLOAD "${_sb_ort_url}" "${_sb_ort_zip}" EXPECTED_HASH "${_sb_ort_hash}" SHOW_PROGRESS)
    file(ARCHIVE_EXTRACT INPUT "${_sb_ort_zip}" DESTINATION "${_sb_ort_dir}")
    file(REMOVE "${_sb_ort_zip}")
  endif()
  file(GLOB _sb_ort_candidates LIST_DIRECTORIES true "${_sb_ort_dir}/*")
  foreach(_sb_ort_cand ${_sb_ort_candidates})
    if(EXISTS "${_sb_ort_cand}/include/onnxruntime_c_api.h")
      set(FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME "${_sb_ort_cand}" CACHE PATH "" FORCE)
    endif()
  endforeach()
  if(NOT FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME)
    set(FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME "${_sb_ort_dir}" CACHE PATH "" FORCE)
  endif()
  message(STATUS "onnxruntime override source dir: ${FETCHCONTENT_SOURCE_DIR_ONNXRUNTIME}")
endif()

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
