# sb_core.cmake — shared isolation rules for every Speech Bridge core.
#
# Each core is a self-contained shared library that statically links its OWN
# engine and its OWN ggml. Only sb_* symbols may be exported; a leaked ggml_*
# (or any non-sb_) symbol must fail `make check-symbols`.
#
# Usage (from cores/<name>/CMakeLists.txt):
#   include(${CMAKE_CURRENT_SOURCE_DIR}/../common/sb_core.cmake)
#   add_library(sb_xxx SHARED ...)
#   sb_core_isolate(sb_xxx)

function(sb_core_isolate tgt)
  set(_common "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")

  set_target_properties(${tgt} PROPERTIES
    C_VISIBILITY_PRESET   hidden
    CXX_VISIBILITY_PRESET  hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE  ON)

  if(APPLE)
    # Hidden visibility + two-level namespace + an explicit export list. ld64
    # does not support -Bsymbolic; it is not needed (two-level namespace binds
    # each core's calls to its own statically-linked ggml).
    target_link_options(${tgt} PRIVATE
      "-Wl,-exported_symbols_list,${_common}/sb_exports.macos.txt")
    set_target_properties(${tgt} PROPERTIES
      MACOSX_RPATH ON
      INSTALL_NAME_DIR "@rpath"
      BUILD_WITH_INSTALL_NAME_DIR ON)
  else()
    target_link_options(${tgt} PRIVATE
      "-Wl,--version-script=${_common}/sb_exports.linux.map"
      "-Wl,-Bsymbolic"
      "-Wl,--exclude-libs,ALL")
    set_target_properties(${tgt} PROPERTIES
      INSTALL_RPATH "$ORIGIN"
      BUILD_RPATH   "$ORIGIN")
  endif()
endfunction()
