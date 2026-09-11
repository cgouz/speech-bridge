cmake_minimum_required(VERSION 3.20)
project(sb_server_cpp LANGUAGES C CXX)

# The C++ orchestrator: HTTP + WebSocket server that dlopens the four
# isolated core libs (libsb_stt/mt/tts_magpie/tts_vits) and runs the batch
# REST + streaming pipeline. Replaces the Go app/ tree entirely — this is
# the ONLY thing that changed; cores/*/ and their vendored engines are
# untouched.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SB_BUILD_TESTS "Build the sb_tests unit-test binary (no models/native libs needed)" ON)

set(THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(UWS_DIR "${THIRD_PARTY_DIR}/uWebSockets")
set(USOCKETS_DIR "${UWS_DIR}/uSockets")

# --- uSockets (C networking layer) --------------------------------------
# Minimal set: no TLS (crypto/), no HTTP/3 (quic.c, io_uring/), no libuv/
# asio/gcd eventing backends. epoll_kqueue.c self-selects via the
# LIBUS_USE_EPOLL/LIBUS_USE_KQUEUE define below. See third_party/PINNED.md.
add_library(usockets STATIC
  "${USOCKETS_DIR}/src/bsd.c"
  "${USOCKETS_DIR}/src/context.c"
  "${USOCKETS_DIR}/src/loop.c"
  "${USOCKETS_DIR}/src/socket.c"
  "${USOCKETS_DIR}/src/udp.c"
  "${USOCKETS_DIR}/src/eventing/epoll_kqueue.c"
)
target_include_directories(usockets PUBLIC "${USOCKETS_DIR}/src")
target_compile_definitions(usockets PUBLIC LIBUS_NO_SSL)
if(APPLE)
  target_compile_definitions(usockets PUBLIC LIBUS_USE_KQUEUE)
else()
  target_compile_definitions(usockets PUBLIC LIBUS_USE_EPOLL)
endif()
set_target_properties(usockets PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- sb_lib: pure C++ logic, no native/dlopen, no uWS ---------------------
# Mirrors app/internal/{text,config,pipeline,obs}/*.go and httpapi/wav.go.
# Depends only on core/core.h's abstract interfaces, never on the concrete
# dlopen-backed implementation — exactly like the Go packages it replaces,
# so `sb_tests` can exercise it with no models and no native libs.
add_library(sb_lib STATIC
  src/text/unicode_lite.cpp
  src/text/split.cpp
  src/text/translit.cpp
  src/config.cpp
  src/obs/log.cpp
  src/obs/metrics.cpp
  src/pipeline/pipeline.cpp
  src/pipeline/audio.cpp
  src/httpapi/wav.cpp
  src/httpapi/multipart.cpp
  src/httpapi/session.cpp
)
target_include_directories(sb_lib PUBLIC
  "${THIRD_PARTY_DIR}/nlohmann_json/include"
  "${UWS_DIR}/src"
)

# --- native shim: dlopen + dlsym the four core libs (pure C) --------------
add_library(sb_native STATIC
  src/native/native_core.cpp
  src/native/shim.c
)
target_include_directories(sb_native PUBLIC
  src/native
  "${CMAKE_CURRENT_SOURCE_DIR}/../cores/common"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cores/stt"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cores/mt"
  "${CMAKE_CURRENT_SOURCE_DIR}/../cores/tts_magpie"
)
if(NOT APPLE)
  target_link_libraries(sb_native PUBLIC dl)
endif()

# --- sb-server binary -----------------------------------------------------
add_executable(sb-server
  src/main.cpp
  src/httpapi/server.cpp
  src/httpapi/handlers.cpp
  src/httpapi/stream.cpp
)
target_include_directories(sb-server PRIVATE
  "${UWS_DIR}/src"
)
find_package(ZLIB REQUIRED)
target_link_libraries(sb-server PRIVATE sb_lib sb_native usockets ZLIB::ZLIB)
if(NOT APPLE)
  target_link_libraries(sb-server PRIVATE pthread)
endif()

install(TARGETS sb-server RUNTIME DESTINATION .)

if(SB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
