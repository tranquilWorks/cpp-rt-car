cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION.txt" RTFW_VERSION LIMIT_COUNT 1)
string(STRIP "${RTFW_VERSION}" RTFW_VERSION)
if (NOT RTFW_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION.txt must contain a semantic version (MAJOR.MINOR.PATCH)")
endif()

project(rtfw VERSION "${RTFW_VERSION}" LANGUAGES C CXX)

set(RTFW_PROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(RTFW_PROJECT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

include(GNUInstallDirs)

find_package(Threads REQUIRED)

set(RTFW_C_ABI_VERSION 8)
set(RTFW_C_ABI_MIN_COMPATIBLE_VERSION 8)

set(
  RTFW_BUILD_ID
  "rtfw-${PROJECT_VERSION}"
  CACHE STRING
  "Stable observability build identifier (A-Za-z0-9._:/@-, max 63 bytes)")
string(LENGTH "${RTFW_BUILD_ID}" RTFW_BUILD_ID_LENGTH)
if (RTFW_BUILD_ID_LENGTH GREATER 63 OR
    NOT RTFW_BUILD_ID MATCHES "^[A-Za-z0-9._:/@-]+$")
  message(FATAL_ERROR
    "RTFW_BUILD_ID must be 1-63 characters from A-Za-z0-9._:/@-")
endif()

# Build the repository tools by default only when RTFW is the top-level
# project. add_subdirectory() consumers receive library targets only.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RTFW_TOP_LEVEL ON)
else()
  set(RTFW_TOP_LEVEL OFF)
endif()

# Options (toggle logging / profiler / tests)
option(ENABLE_LOG "Enable logging" ON)
option(ENABLE_PROF "Enable profiler" ON)
if (RTFW_TOP_LEVEL)
  # Legacy top-level spelling retained for existing presets and automation.
  option(ENABLE_TESTS "Build RTFW tests" ON)
  # Keep it authoritative on every reconfigure; do not leave a second cache
  # entry that can drift from the documented legacy switch.
  set(RTFW_BUILD_TESTS "${ENABLE_TESTS}")
else()
  # A parent's generic ENABLE_TESTS cache entry must not opt this subproject
  # into dependency discovery, test registration, or global test targets.
  option(
    RTFW_BUILD_TESTS
    "Build RTFW tests when used through add_subdirectory"
    OFF)
endif()
option(ENABLE_RAPIDCHECK "Enable RapidCheck property tests" OFF)
option(SIM_BUILD_FUZZERS "Build the Clang libFuzzer harness" OFF)
option(SIM_ENABLE_IWYU "Run include-what-you-use during builds" OFF)
option(SIM_WARN_ODR "Enable -Wodr to detect ODR violations" OFF)
option(
  RTFW_BUILD_EXPERIMENTAL
  "Build unsupported SimCore, scheduler, plugin, and utility experiments"
  OFF)
option(
  RTFW_INSTALL_EXPERIMENTAL
  "Install unsupported research targets and headers"
  OFF)
option(
  RTFW_BUILD_EXAMPLES
  "Build embedding and backend examples"
  "${RTFW_TOP_LEVEL}")
option(
  RTFW_BUILD_BENCHMARKS
  "Build the optional supported host-side benchmark framework"
  OFF)
option(
  RTFW_BUILD_RUNTIME_DEMO
  "Build and install the supported profile-driven runtime demo"
  "${RTFW_TOP_LEVEL}")
option(
  RTFW_ENABLE_CUDA
  "Build the optional CUDA Driver API adapter and hardware qualification tool"
  OFF)
option(
  RTFW_ENABLE_XDMA
  "Build the Linux Xilinx XDMA character-device adapter and qualification tool"
  OFF)

if (RTFW_INSTALL_EXPERIMENTAL AND NOT RTFW_BUILD_EXPERIMENTAL)
  message(FATAL_ERROR
    "RTFW_INSTALL_EXPERIMENTAL requires RTFW_BUILD_EXPERIMENTAL=ON")
endif()

# Sanitizer toggles (semicolons to enable multiple: address;thread;leak;memory;hwaddress;safe-stack;cfi)
set(SIM_SANITIZERS "" CACHE STRING "Sanitizers to enable for all targets")

# Hardened build switches
option(SIM_WERROR "Treat warnings as errors in this project's targets" OFF)
option(SIM_ENABLE_LTO "Enable link time optimization" OFF)
option(SIM_LTO_THIN "Use ThinLTO when LTO is enabled" OFF)
set(SIM_PGO "" CACHE STRING "Profile guided optimisation mode: gen or use")
set(SIM_PGO_PROFILE "" CACHE FILEPATH "Profile data path for PGO")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizers
if (SIM_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(STATUS "Enabling sanitizers: ${SIM_SANITIZERS}")
  string(REPLACE ";" "," _san_flags "${SIM_SANITIZERS}")
  add_compile_options(-fsanitize=${_san_flags} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${_san_flags} -fno-omit-frame-pointer)
  if (SIM_SANITIZERS MATCHES "cfi")
    add_compile_options(-flto)
    add_link_options(-flto)
  endif()
endif()

# Helper to link shared objects against sanitizer runtimes when we need to
# satisfy the dynamic loader (e.g. for dlopen-based smoke tests).
function(simcore_link_sanitizer_runtime target)
  if (NOT SIM_SANITIZERS)
    return()
  endif()
  if (MSVC)
    return()
  endif()
  if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    return()
  endif()

  set(_runtime_tags)
  foreach(_san IN LISTS SIM_SANITIZERS)
    if (_san STREQUAL "address" OR _san STREQUAL "leak")
      list(APPEND _runtime_tags asan)
    elseif (_san STREQUAL "thread")
      list(APPEND _runtime_tags tsan)
    elseif (_san STREQUAL "memory")
      list(APPEND _runtime_tags msan)
    elseif (_san STREQUAL "undefined")
      list(APPEND _runtime_tags ubsan)
    elseif (_san STREQUAL "hwaddress")
      list(APPEND _runtime_tags hwasan)
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _runtime_tags)
  if (NOT _runtime_tags)
    return()
  endif()

  list(JOIN SIM_SANITIZERS "," _san_flags)

  set(_runtime_patterns)
  foreach(_tag IN LISTS _runtime_tags)
    if (_tag STREQUAL "asan")
      list(APPEND _runtime_patterns "asan|asan")
    elseif (_tag STREQUAL "ubsan")
      list(APPEND _runtime_patterns "ubsan|ubsan")
    elseif (_tag STREQUAL "tsan")
      list(APPEND _runtime_patterns "tsan|tsan")
    elseif (_tag STREQUAL "msan")
      list(APPEND _runtime_patterns "msan|msan")
    elseif (_tag STREQUAL "hwasan")
      list(APPEND _runtime_patterns "hwasan|hwasan")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _runtime_patterns)
  if (NOT _runtime_patterns)
    return()
  endif()

  set(_handled_tags)
  set(_runtime_entries)
  if (CMAKE_CXX_COMPILER)
    set(_dummy_dir "${RTFW_PROJECT_BINARY_DIR}/CMakeFiles")
    file(MAKE_DIRECTORY "${_dummy_dir}")
    set(_dummy_src "${_dummy_dir}/simcore_sanitize_dummy.cpp")
    if (NOT EXISTS "${_dummy_src}")
      file(WRITE "${_dummy_src}" "int simcore_sanitize_dummy() { return 0; }\n")
    endif()
    set(_dummy_out "${_dummy_dir}/libsimcore_sanitize_dummy.so")
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} "-###" "-x" "c++" "-" "-shared" "-fsanitize=${_san_flags}" "-fno-omit-frame-pointer" "-o" "${_dummy_out}"
      INPUT_FILE "${_dummy_src}"
      OUTPUT_VARIABLE _driver_stdout
      ERROR_VARIABLE _driver_stderr
      RESULT_VARIABLE _driver_result
    )
    if (_driver_result EQUAL 0)
      set(_driver_output "${_driver_stdout}\n${_driver_stderr}")
      string(REGEX MATCHALL "\"([^\"]+)\"" _quoted_tokens "${_driver_output}")
      set(_in_whole OFF)
      foreach(_token IN LISTS _quoted_tokens)
        string(REGEX REPLACE "^\"|\"$" "" _value "${_token}")
        if (_value MATCHES "simcore_sanitize_dummy")
          continue()
        endif()
        if (_value MATCHES "^-fsanitize")
          continue()
        endif()
        if (_value STREQUAL "--whole-archive" OR _value STREQUAL "-Wl,--whole-archive")
          set(_in_whole ON)
          continue()
        elseif (_value STREQUAL "--no-whole-archive" OR _value STREQUAL "-Wl,--no-whole-archive")
          set(_in_whole OFF)
          continue()
        endif()
        foreach(_pattern_entry IN LISTS _runtime_patterns)
          string(REPLACE "|" ";" _pattern_parts "${_pattern_entry}")
          list(GET _pattern_parts 0 _pattern_tag)
          list(GET _pattern_parts 1 _pattern_regex)
          if (_value MATCHES "${_pattern_regex}")
            list(APPEND _handled_tags "${_pattern_tag}")
            if (_pattern_tag STREQUAL "asan" AND _value MATCHES "asan_static")
              break()
            endif()
            if (_in_whole)
              list(APPEND _runtime_entries "WHOLE|${_value}")
            else()
              list(APPEND _runtime_entries "NORMAL|${_value}")
            endif()
            break()
          endif()
        endforeach()
      endforeach()
    endif()
  endif()

  list(REMOVE_DUPLICATES _handled_tags)

  set(_missing_tags)
  foreach(_tag IN LISTS _runtime_tags)
    if (NOT _tag IN_LIST _handled_tags)
      list(APPEND _missing_tags "${_tag}")
    endif()
  endforeach()

  set(_used_fallback OFF)
  if (_missing_tags)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch_lower)
    if (_arch_lower STREQUAL "x86_64" OR _arch_lower STREQUAL "amd64")
      set(_arch_token "x86_64")
    elseif (_arch_lower STREQUAL "aarch64" OR _arch_lower STREQUAL "arm64")
      set(_arch_token "aarch64")
    else()
      set(_arch_token "${_arch_lower}")
    endif()

    foreach(_tag IN LISTS _missing_tags)
      set(_gnu_candidates)
      set(_clang_candidates)
      if (_tag STREQUAL "asan")
        set(_gnu_candidates
          "libasan.so"
          "libasan.a"
        )
        set(_clang_candidates
          "libclang_rt.asan-${_arch_token}.so"
          "libclang_rt.asan-${_arch_token}.a"
          "libclang_rt.asan_cxx-${_arch_token}.so"
          "libclang_rt.asan_cxx-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "ubsan")
        set(_gnu_candidates
          "libubsan.so"
          "libubsan.a"
        )
        set(_clang_candidates
          "libclang_rt.ubsan_standalone-${_arch_token}.so"
          "libclang_rt.ubsan_standalone-${_arch_token}.a"
          "libclang_rt.ubsan_standalone_cxx-${_arch_token}.so"
          "libclang_rt.ubsan_standalone_cxx-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "tsan")
        set(_gnu_candidates
          "libtsan.so"
          "libtsan.a"
        )
        set(_clang_candidates
          "libclang_rt.tsan-${_arch_token}.so"
          "libclang_rt.tsan-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "msan")
        set(_clang_candidates
          "libclang_rt.msan-${_arch_token}.so"
          "libclang_rt.msan-${_arch_token}.a"
        )
      elseif (_tag STREQUAL "hwasan")
        set(_clang_candidates
          "libclang_rt.hwasan-${_arch_token}.so"
          "libclang_rt.hwasan-${_arch_token}.a"
        )
      endif()

      set(_candidates)
      if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        list(APPEND _candidates ${_clang_candidates} ${_gnu_candidates})
      else()
        list(APPEND _candidates ${_gnu_candidates} ${_clang_candidates})
      endif()

      foreach(_candidate IN LISTS _candidates)
        execute_process(
          COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_candidate}
          OUTPUT_VARIABLE _candidate_path
          OUTPUT_STRIP_TRAILING_WHITESPACE
          ERROR_QUIET
        )
        if (_candidate_path AND NOT _candidate_path STREQUAL ${_candidate}
            AND EXISTS "${_candidate_path}")
          set(_entry_flag "NORMAL")
          if (_candidate_path MATCHES "_static" AND _candidate_path MATCHES "\\.a$")
            set(_entry_flag "WHOLE")
          endif()
          list(APPEND _runtime_entries "${_entry_flag}|${_candidate_path}")
          list(APPEND _handled_tags "${_tag}")
          break()
        endif()
      endforeach()
    endforeach()
    if (_runtime_entries)
      set(_used_fallback ON)
    endif()
  endif()

  list(REMOVE_DUPLICATES _handled_tags)

  if (NOT _runtime_entries)
    return()
  endif()

  set(_link_directives)
  set(_seen_libs)
  foreach(_entry IN LISTS _runtime_entries)
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _entry_flag)
    list(GET _parts 1 _lib)

    set(_needs_whole OFF)
    if (_entry_flag STREQUAL "WHOLE")
      set(_needs_whole ON)
    endif()

    if (_lib MATCHES "^-Wl,")
      list(APPEND _link_directives "${_lib}")
      continue()
    elseif (_lib MATCHES "^-l.+")
      list(APPEND _link_directives "${_lib}")
      continue()
    elseif (NOT (IS_ABSOLUTE "${_lib}" AND EXISTS "${_lib}"))
      continue()
    endif()

    set(_final_path)
    set(_final_is_static OFF)
    if (_lib MATCHES "\\.a$")
      get_filename_component(_lib_dir "${_lib}" DIRECTORY)
      get_filename_component(_lib_name "${_lib}" NAME_WE)

      set(_stem_variants "${_lib_name}")
      if (_lib_name MATCHES "_static(-|$)")
        string(REGEX REPLACE "_static(-|$)" "\\1" _no_static "${_lib_name}")
        list(APPEND _stem_variants "${_no_static}")
      endif()
      list(REMOVE_DUPLICATES _stem_variants)

      set(_shared_match)
      foreach(_stem IN LISTS _stem_variants)
        file(GLOB _maybe_shared "${_lib_dir}/${_stem}.so" "${_lib_dir}/${_stem}.so.*")
        if (_maybe_shared)
          list(SORT _maybe_shared)
          list(GET _maybe_shared 0 _shared_match)
          break()
        endif()
        file(GLOB _maybe_dylib "${_lib_dir}/${_stem}.dylib" "${_lib_dir}/${_stem}.dylib.*")
        if (_maybe_dylib)
          list(SORT _maybe_dylib)
          list(GET _maybe_dylib 0 _shared_match)
          break()
        endif()
      endforeach()

      if (_shared_match)
        set(_final_path "${_shared_match}")
      else()
        set(_final_path "${_lib}")
        set(_final_is_static ON)
      endif()
    else()
      set(_final_path "${_lib}")
    endif()

    if (_final_path AND NOT _final_path IN_LIST _seen_libs)
      list(APPEND _seen_libs "${_final_path}")
      if (_final_is_static AND _needs_whole)
        list(APPEND _link_directives "-Wl,--whole-archive" "${_final_path}" "-Wl,--no-whole-archive")
      else()
        list(APPEND _link_directives "${_final_path}")
      endif()
    endif()
  endforeach()

  if (_link_directives)
    target_link_libraries(${target} PRIVATE ${_link_directives})
    if (_used_fallback)
      message(STATUS "Sanitizer runtimes for ${target}: using fallback ${_link_directives}")
    else()
      message(STATUS "Sanitizer runtimes for ${target}: ${_link_directives}")
    endif()
  endif()
endfunction()

# IWYU
if (SIM_ENABLE_IWYU)
  find_program(IWYU_PATH NAMES include-what-you-use iwyu)
  if (IWYU_PATH)
    set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${IWYU_PATH})
  else()
    message(WARNING "include-what-you-use requested but not found")
  endif()
endif()

# ODR warnings
if (SIM_WARN_ODR AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wodr)
endif()

# LTO
if (SIM_ENABLE_LTO)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if (SIM_LTO_THIN)
      add_compile_options(-flto=thin)
      add_link_options(-flto=thin)
    else()
      add_compile_options(-flto)
      add_link_options(-flto)
    endif()
  elseif (MSVC)
    add_compile_options(/GL)
    add_link_options(/LTCG)
  endif()
endif()

# PGO
if (SIM_PGO STREQUAL "gen")
  add_compile_options(-fprofile-generate=${SIM_PGO_PROFILE})
  add_link_options(-fprofile-generate=${SIM_PGO_PROFILE})
elseif(SIM_PGO STREQUAL "use")
  add_compile_options(-fprofile-use=${SIM_PGO_PROFILE} -fprofile-correction)
  add_link_options(-fprofile-use=${SIM_PGO_PROFILE} -fprofile-correction)
endif()


# --- GoogleTest integration ---
# Use FetchContent to obtain googletest at configure time instead of
# expecting a checked out submodule. This keeps the repository light and
# avoids build failures when the submodule is missing.
if (RTFW_BUILD_TESTS)
    option(SIM_DOWNLOAD_GTEST "Download GoogleTest if it is not provided" ON)
    set(SIM_GTEST_AVAILABLE OFF)

    if (EXISTS
        "${RTFW_PROJECT_SOURCE_DIR}/external/googletest/CMakeLists.txt")
        add_subdirectory(
            "${RTFW_PROJECT_SOURCE_DIR}/external/googletest"
            "${RTFW_PROJECT_BINARY_DIR}/_deps/googletest")
        set(SIM_GTEST_AVAILABLE ON)
        set(SIM_GTEST_LIBRARIES gtest gtest_main gmock)
    else()
        find_package(GTest CONFIG QUIET)
        if (GTest_FOUND)
            set(SIM_GTEST_AVAILABLE ON)
            set(SIM_GTEST_LIBRARIES GTest::gtest)
            if (TARGET GTest::gtest_main)
                list(APPEND SIM_GTEST_LIBRARIES GTest::gtest_main)
            endif()
            if (TARGET GTest::gmock)
                list(APPEND SIM_GTEST_LIBRARIES GTest::gmock)
            endif()
        elseif (SIM_DOWNLOAD_GTEST)
            include(FetchContent)
            FetchContent_Declare(
                googletest
                URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
                URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            )
            # For Windows: Prevent overriding the parent project's compiler/linker
            # settings on MSVC.
            set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
            FetchContent_MakeAvailable(googletest)
            set(SIM_GTEST_AVAILABLE ON)
            set(SIM_GTEST_LIBRARIES)
            foreach(gtest_target gtest gtest_main gmock)
                if (TARGET ${gtest_target})
                    list(APPEND SIM_GTEST_LIBRARIES ${gtest_target})
                endif()
            endforeach()
            if (NOT TARGET gtest)
                message(FATAL_ERROR
                    "The GoogleTest source override did not provide gtest")
            endif()
        else()
            message(WARNING "GoogleTest not available; gtest-based tests will be skipped.")
        endif()
    endif()

    if (ENABLE_RAPIDCHECK)
        include(FetchContent)
        FetchContent_Declare(
            rapidcheck
            GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
            GIT_TAG 6e8dadfdafa3a74eabb52ead87f8787f72eccd0b
        )
        FetchContent_MakeAvailable(rapidcheck)
    endif()
endif()

function(rtfw_enable_project_warnings target)
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor)
    if (SIM_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  elseif (MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if (SIM_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  endif()
endfunction()

if (RTFW_BUILD_EXPERIMENTAL)
  # Header-only research surface. Its feature macros stay build-local and are
  # never part of a supported installed target's usage requirements.
  add_library(simcore INTERFACE)
  target_include_directories(simcore INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/hal>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gpu>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  target_link_libraries(simcore INTERFACE Threads::Threads)
  target_compile_features(simcore INTERFACE cxx_std_20)
  if (ENABLE_LOG)
    target_compile_definitions(
      simcore INTERFACE
      $<BUILD_INTERFACE:LOG_ENABLED>
      $<BUILD_INTERFACE:LOG_DEFAULT_LEVEL=2>)
  endif()
  if (ENABLE_PROF)
    target_compile_definitions(
      simcore INTERFACE $<BUILD_INTERFACE:PROF_ENABLED>)
  endif()

  add_library(simcore_platform STATIC src/platform_init.cpp src/metrics.cpp)
  target_include_directories(simcore_platform PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(simcore_platform PUBLIC simcore)
  set_target_properties(simcore_platform PROPERTIES
      CXX_VISIBILITY_PRESET hidden
      POSITION_INDEPENDENT_CODE ON
      VISIBILITY_INLINES_HIDDEN YES)
  rtfw_enable_project_warnings(simcore_platform)

  add_library(rt_memory INTERFACE)
  target_include_directories(
    rt_memory INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

  # Core module retained only for research/compatibility headers.
  add_library(simcore_core INTERFACE)
  target_include_directories(simcore_core INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
endif()

# In your top-level CMakeLists.txt

# Ensure asserts are ON for tests (avoid -DNDEBUG)
if (RTFW_BUILD_TESTS AND CMAKE_BUILD_TYPE MATCHES "Release")
  message(WARNING "Tests are more useful with assertions; consider Debug or RelWithDebInfo.")
endif()

# Optional toggles:
# target_compile_definitions(rt_memory INTERFACE RT_ARENA_PRETOUCH=1)
# target_compile_definitions(rt_memory INTERFACE RT_ARENA_MLOCK=1)
# target_compile_definitions(rt_memory INTERFACE RT_ARENA_MADVISE_HUGEPAGE=1)

# --- Tests ---



option(SIM_ENABLE_AVX2 "Enable experimental global AVX2/FMA compilation" OFF)

if (SIM_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  add_compile_options(-mavx2 -mfma)
endif()

# Supported runtime implementation. This is the only C++ execution library
# behind the stable C libraries and the preferred installed C++ target.
set(RTFW_RUNTIME_SOURCES
    rt/src/compiled_graph.cpp
    rt/src/cross_rate_data.cpp
    rt/src/command_batch.cpp
    rt/src/hal_v2.cpp
    rt/src/heterogeneous_memory.cpp
    rt/src/device_manager.cpp
    rt/src/executor.cpp
    rt/src/extension_registration.cpp
    rt/src/host_runtime.cpp
    rt/src/live_control_actions.cpp
    rt/src/live_control_mailbox.cpp
    rt/src/live_control_replay.cpp
    rt/src/memory_policy.cpp
    rt/src/mixed_rate_actions.cpp
    rt/src/mixed_rate_replay.cpp
    rt/src/mock_device.cpp
    rt/src/native_platform_preflight.cpp
    rt/src/observability_export.cpp
    rt/src/rate_dispatch.cpp
    rt/src/rate_telemetry.cpp
    rt/src/rate_timeline.cpp
    rt/src/resource_policy.cpp
    rt/src/runtime_compat.cpp
    rt/src/runtime_profile.cpp
    rt/src/sampled_io.cpp
    rt/src/loopback_backend.cpp
    rt/src/snapshot_codec.cpp
    rt/src/telemetry.cpp
    rt/src/thread_policy.cpp
    rt/src/watchdog_monitor.cpp
)

add_library(rtfw_runtime STATIC ${RTFW_RUNTIME_SOURCES})
target_compile_definitions(
    rtfw_runtime
    PRIVATE RTFW_BUILD_ID_STRING="${RTFW_BUILD_ID}")
target_compile_features(rtfw_runtime PUBLIC cxx_std_20)
target_include_directories(rtfw_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(rtfw_runtime PUBLIC Threads::Threads)
set_target_properties(rtfw_runtime PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    EXPORT_NAME runtime
    OUTPUT_NAME rtfw_runtime
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN YES)
rtfw_enable_project_warnings(rtfw_runtime)
add_library(rtfw::runtime ALIAS rtfw_runtime)

# Narrow 1.x compatibility library. It keeps declarations that accidentally
# became reachable from runtime.hpp linkable without putting their research
# implementation in the supported runtime archive.
add_library(
    simcore_rt STATIC
    rt/src/runtime.cpp
    rt/src/fiber_pool.cpp)
target_compile_features(simcore_rt PUBLIC cxx_std_20)
target_include_directories(simcore_rt PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/hal>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gpu>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(simcore_rt PUBLIC rtfw_runtime Threads::Threads)
set_target_properties(simcore_rt PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    EXPORT_NAME simcore_rt
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN YES)
rtfw_enable_project_warnings(simcore_rt)
add_library(rtfw::simcore_rt ALIAS simcore_rt)

if (RTFW_BUILD_EXPERIMENTAL)
  set(RTFW_EXPERIMENTAL_SOURCES
      rt/src/plugin_manager.cpp
      rt/src/scheduler.cpp)
  if (UNIX)
    list(APPEND RTFW_EXPERIMENTAL_SOURCES rt/src/crashdump_posix.cpp)
  else()
    list(APPEND RTFW_EXPERIMENTAL_SOURCES rt/src/crashdump_stub.cpp)
  endif()
  add_library(rtfw_experimental STATIC ${RTFW_EXPERIMENTAL_SOURCES})
  target_compile_features(rtfw_experimental PUBLIC cxx_std_20)
  target_include_directories(rtfw_experimental PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(
      rtfw_experimental PUBLIC simcore simcore_rt Threads::Threads)
  if (UNIX)
    target_link_libraries(rtfw_experimental PRIVATE dl)
  endif()
  set_target_properties(rtfw_experimental PROPERTIES
      CXX_VISIBILITY_PRESET hidden
      EXPORT_NAME experimental
      OUTPUT_NAME rtfw_experimental
      POSITION_INDEPENDENT_CODE ON
      VISIBILITY_INLINES_HIDDEN YES)
  rtfw_enable_project_warnings(rtfw_experimental)
  add_library(rtfw::experimental ALIAS rtfw_experimental)
endif()

# CUDA backend state machine. This target deliberately has no toolkit
# dependency so its bounded behavior remains testable on CPU-only builders.
add_library(rtfw_cuda_backend STATIC rt/src/cuda_backend.cpp)
target_compile_features(rtfw_cuda_backend PUBLIC cxx_std_20)
target_include_directories(rtfw_cuda_backend PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(rtfw_cuda_backend PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    EXPORT_NAME cuda_backend
    OUTPUT_NAME rtfw_cuda_backend
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN YES
)
rtfw_enable_project_warnings(rtfw_cuda_backend)
add_library(rtfw::cuda_backend ALIAS rtfw_cuda_backend)

# Vendor headers and symbols stay in a separate opt-in adapter.
if (RTFW_ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    add_library(rtfw_cuda_driver STATIC rt/src/cuda_driver.cpp)
    target_include_directories(rtfw_cuda_driver PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_definitions(
        rtfw_cuda_driver
        PUBLIC RTFW_CUDA_DRIVER_AVAILABLE=1)
    target_link_libraries(
        rtfw_cuda_driver
        PUBLIC rtfw_cuda_backend CUDA::cuda_driver)
    set_target_properties(rtfw_cuda_driver PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        EXPORT_NAME cuda_driver
        OUTPUT_NAME rtfw_cuda_driver
        POSITION_INDEPENDENT_CODE ON
        VISIBILITY_INLINES_HIDDEN YES
    )
    rtfw_enable_project_warnings(rtfw_cuda_driver)
    add_library(rtfw::cuda_driver ALIAS rtfw_cuda_driver)
endif()

# XDMA backend state machine. Blocking driver transfers execute only on its
# fixed initialization-time I/O workers, keeping submit/poll bounded and
# portable for ordinary CI.
add_library(rtfw_xdma_backend STATIC rt/src/xdma_backend.cpp)
target_compile_features(rtfw_xdma_backend PUBLIC cxx_std_20)
target_include_directories(rtfw_xdma_backend PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(
    rtfw_xdma_backend PUBLIC Threads::Threads)
set_target_properties(rtfw_xdma_backend PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    EXPORT_NAME xdma_backend
    OUTPUT_NAME rtfw_xdma_backend
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN YES
)
rtfw_enable_project_warnings(rtfw_xdma_backend)
add_library(rtfw::xdma_backend ALIAS rtfw_xdma_backend)

if (RTFW_ENABLE_XDMA)
    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "RTFW_ENABLE_XDMA supports only the named Linux XDMA stack")
    endif()
    add_library(rtfw_xdma_linux STATIC rt/src/xdma_linux.cpp)
    target_compile_features(rtfw_xdma_linux PUBLIC cxx_std_20)
    target_include_directories(rtfw_xdma_linux PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    target_compile_definitions(
        rtfw_xdma_linux
        PUBLIC RTFW_XDMA_LINUX_AVAILABLE=1)
    target_link_libraries(
        rtfw_xdma_linux PUBLIC rtfw_xdma_backend)
    set_target_properties(rtfw_xdma_linux PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        EXPORT_NAME xdma_linux
        OUTPUT_NAME rtfw_xdma_linux
        POSITION_INDEPENDENT_CODE ON
        VISIBILITY_INLINES_HIDDEN YES
    )
    rtfw_enable_project_warnings(rtfw_xdma_linux)
    add_library(rtfw::xdma_linux ALIAS rtfw_xdma_linux)
endif()

set(RTFW_SOURCES src/c_abi.cpp)

add_library(rtfw_shared SHARED ${RTFW_SOURCES})
add_library(rtfw_static STATIC ${RTFW_SOURCES})

foreach(lib rtfw_shared rtfw_static)
    target_include_directories(${lib}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/rt/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    set_target_properties(${lib} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
        VISIBILITY_INLINES_HIDDEN YES)
    rtfw_enable_project_warnings(${lib})
endforeach()

target_compile_definitions(rtfw_shared PRIVATE RTFW_BUILD=1)
target_compile_definitions(rtfw_static PUBLIC RTFW_STATIC_DEFINE=1)

set_target_properties(rtfw_shared PROPERTIES
    OUTPUT_NAME rtfw
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${RTFW_C_ABI_VERSION}"
)
set_target_properties(rtfw_static PROPERTIES OUTPUT_NAME rtfw_static)

set(
    RTFW_ABI_EXPORTS_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/abi/rtfw_c_abi_v8.exports")
file(STRINGS "${RTFW_ABI_EXPORTS_FILE}" RTFW_C_ABI_EXPORTS
    REGEX "^[A-Za-z_][A-Za-z0-9_]*$")
if (NOT RTFW_C_ABI_EXPORTS)
    message(FATAL_ERROR "The C ABI export allowlist is empty")
endif()

if (UNIX AND NOT APPLE)
    set(RTFW_ELF_VERSION_SCRIPT
        "${CMAKE_CURRENT_BINARY_DIR}/rtfw-c-abi.map")
    file(WRITE "${RTFW_ELF_VERSION_SCRIPT}"
        "RTFW_${RTFW_C_ABI_VERSION} {\n  global:\n")
    foreach(symbol IN LISTS RTFW_C_ABI_EXPORTS)
        file(APPEND "${RTFW_ELF_VERSION_SCRIPT}" "    ${symbol};\n")
    endforeach()
    file(APPEND "${RTFW_ELF_VERSION_SCRIPT}" "  local: *;\n};\n")
    target_link_options(
        rtfw_shared
        PRIVATE "LINKER:--version-script=${RTFW_ELF_VERSION_SCRIPT}")
    set_property(
        TARGET rtfw_shared
        APPEND PROPERTY LINK_DEPENDS
        "${RTFW_ELF_VERSION_SCRIPT};${RTFW_ABI_EXPORTS_FILE}")
elseif (APPLE)
    set(RTFW_MACHO_EXPORT_LIST
        "${CMAKE_CURRENT_BINARY_DIR}/rtfw-c-abi.exports")
    file(WRITE "${RTFW_MACHO_EXPORT_LIST}" "")
    foreach(symbol IN LISTS RTFW_C_ABI_EXPORTS)
        file(APPEND "${RTFW_MACHO_EXPORT_LIST}" "_${symbol}\n")
    endforeach()
    target_link_options(
        rtfw_shared
        PRIVATE "LINKER:-exported_symbols_list,${RTFW_MACHO_EXPORT_LIST}")
    set_property(
        TARGET rtfw_shared
        APPEND PROPERTY LINK_DEPENDS
        "${RTFW_MACHO_EXPORT_LIST};${RTFW_ABI_EXPORTS_FILE}")
endif()

if (MSVC)
    # Give each binary its own program database to avoid clashes when
    # multi-config generators build both variants in the same output tree.
    set_target_properties(rtfw_shared PROPERTIES
        COMPILE_PDB_NAME rtfw_shared
        PDB_NAME rtfw_shared
    )
    set_target_properties(rtfw_static PROPERTIES
        COMPILE_PDB_NAME rtfw_static
    )
endif()

target_link_libraries(rtfw_shared PRIVATE rtfw_runtime)
target_link_libraries(rtfw_static PRIVATE rtfw_runtime)

# A C-only project still needs the implementation toolchain's C++ support
# libraries when it selects the static C API. Export only the C++-specific
# portion; libraries already implicit for the C driver remain implicit.
set(RTFW_CXX_RUNTIME_LIBRARIES ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES})
foreach(runtime_library IN LISTS CMAKE_C_IMPLICIT_LINK_LIBRARIES)
    list(REMOVE_ITEM RTFW_CXX_RUNTIME_LIBRARIES "${runtime_library}")
endforeach()
list(REMOVE_DUPLICATES RTFW_CXX_RUNTIME_LIBRARIES)
if (RTFW_CXX_RUNTIME_LIBRARIES)
    target_link_libraries(
        rtfw_static INTERFACE ${RTFW_CXX_RUNTIME_LIBRARIES})
endif()

set_target_properties(rtfw_shared PROPERTIES EXPORT_NAME rtfw)
add_library(rtfw ALIAS rtfw_shared)
add_library(rtfw::rtfw ALIAS rtfw_shared)
add_library(rtfw::rtfw_static ALIAS rtfw_static)

add_library(rtfw_c_shared INTERFACE)
target_link_libraries(rtfw_c_shared INTERFACE rtfw_shared)
set_target_properties(rtfw_c_shared PROPERTIES EXPORT_NAME c_shared)
add_library(rtfw::c_shared ALIAS rtfw_c_shared)

add_library(rtfw_c_static INTERFACE)
target_link_libraries(rtfw_c_static INTERFACE rtfw_static)
set_target_properties(rtfw_c_static PROPERTIES EXPORT_NAME c_static)
add_library(rtfw::c_static ALIAS rtfw_c_static)

simcore_link_sanitizer_runtime(rtfw_shared)

if (RTFW_BUILD_EXPERIMENTAL)
  # Legacy demonstration executable retained for repository regression only.
  add_executable(simcore_app src/main.cpp)
  target_link_libraries(simcore_app PRIVATE simcore_platform simcore_rt)
  set_target_properties(simcore_app PROPERTIES OUTPUT_NAME rtfw_demo)
  add_executable(rtfw_demo ALIAS simcore_app)
  rtfw_enable_project_warnings(simcore_app)

  # Sample trace dump utility for the legacy binary trace.
  add_executable(trace_dump src/trace_dump.cpp)
  target_link_libraries(trace_dump PRIVATE simcore_platform)
  target_include_directories(trace_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  rtfw_enable_project_warnings(trace_dump)
endif()

if (RTFW_BUILD_RUNTIME_DEMO)
  # Profile-driven target-runtime demo and autotune workload.
  add_executable(rtfw_runtime_demo src/runtime_profile_demo.cpp)
  target_link_libraries(rtfw_runtime_demo PRIVATE rtfw_runtime)
  rtfw_enable_project_warnings(rtfw_runtime_demo)
endif()

if (RTFW_BUILD_TESTS)
    enable_testing()
endif()
if (RTFW_BUILD_EXAMPLES)
  add_subdirectory(samples)
endif()

if (RTFW_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

# Tests
if (RTFW_BUILD_TESTS)
    add_subdirectory(tests)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeterminismTargets.cmake)
endif()

# Optional Bloaty size analysis
find_program(BLOATY_EXE NAMES bloaty)
if (BLOATY_EXE AND TARGET simcore_app)
  add_custom_target(bloaty
    COMMAND ${BLOATY_EXE} $<TARGET_FILE:simcore_app> -d compileunits,symbols -n 0.1
    DEPENDS simcore_app
    COMMENT "Run Bloaty on simcore_app")
endif()

if (RTFW_BUILD_TESTS)
  add_custom_target(rtfw_tests
      COMMAND
          ${CMAKE_CTEST_COMMAND}
          --test-dir ${RTFW_PROJECT_BINARY_DIR}
          --output-on-failure
      WORKING_DIRECTORY ${RTFW_PROJECT_BINARY_DIR}
      COMMENT "Run configured tests")
endif()

include(CMakePackageConfigHelpers)

set(RTFW_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/rtfw")

install(
    TARGETS
        rtfw_shared
        rtfw_static
        rtfw_c_shared
        rtfw_c_static
        rtfw_runtime
        simcore_rt
        rtfw_cuda_backend
        rtfw_xdma_backend
    EXPORT rtfwTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if (RTFW_BUILD_RUNTIME_DEMO)
  install(
      TARGETS rtfw_runtime_demo
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if (RTFW_ENABLE_CUDA)
    install(
        TARGETS rtfw_cuda_driver
        EXPORT rtfwCudaTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

if (RTFW_ENABLE_XDMA)
    install(
        TARGETS rtfw_xdma_linux
        EXPORT rtfwXdmaTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

# The default SDK surface is intentionally explicit. Internal and research
# headers may exist in the source tree without becoming an accidental API.
install(
    FILES include/rtfw/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtfw)
install(
    FILES
        rt/include/rt/c_api.h
        rt/include/rt/device_abi.h
        rt/include/rt/extension_abi.h
        rt/include/rt/canonical_bytes.hpp
        rt/include/rt/config.hpp
        rt/include/rt/device.hpp
        rt/include/rt/graph.hpp
        rt/include/rt/mock_device.hpp
        rt/include/rt/observability_export.hpp
        rt/include/rt/profile.hpp
        rt/include/rt/runtime.hpp
        rt/include/rt/loopback_backend.hpp
        rt/include/rt/live_control.hpp
        rt/include/rt/sdk.hpp
        rt/include/rt/status.hpp
        rt/include/rt/version.hpp
        rt/include/rt/cuda_backend.hpp
        rt/include/rt/xdma_backend.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rt)
install(
    FILES core/include/core/units.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/core)

if (RTFW_ENABLE_CUDA)
  install(
      FILES rt/include/rt/cuda_driver.hpp
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rt)
endif()

if (RTFW_ENABLE_XDMA)
  install(
      FILES rt/include/rt/xdma_linux.hpp
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rt)
endif()

if (RTFW_INSTALL_EXPERIMENTAL)
  install(
      TARGETS
          simcore
          simcore_core
          simcore_platform
          rt_memory
          rtfw_experimental
      EXPORT rtfwExperimentalTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(
      DIRECTORY include/ rt/include/ core/include/
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
      FILES_MATCHING
          PATTERN "*.h"
          PATTERN "*.hh"
          PATTERN "*.hpp"
          PATTERN "*.hxx"
          PATTERN "*.inl"
          PATTERN "*.ipp"
          PATTERN "*.tpp")
  install(
      FILES api/cabi.h
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/api)
  install(
      DIRECTORY hal/
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hal
      FILES_MATCHING
          PATTERN "*.h"
          PATTERN "*.hh"
          PATTERN "*.hpp"
          PATTERN "*.hxx"
          PATTERN "*.inl"
          PATTERN "*.ipp"
          PATTERN "*.tpp")
  install(
      DIRECTORY gpu/
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gpu
      FILES_MATCHING
          PATTERN "*.h"
          PATTERN "*.hh"
          PATTERN "*.hpp"
          PATTERN "*.hxx"
          PATTERN "*.inl"
          PATTERN "*.ipp"
          PATTERN "*.tpp")
endif()

configure_package_config_file(
    cmake/rtfwConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/rtfwConfig.cmake
    INSTALL_DESTINATION ${RTFW_INSTALL_CMAKEDIR}
    PATH_VARS
        CMAKE_INSTALL_INCLUDEDIR
        CMAKE_INSTALL_DATADIR
)

if (PROJECT_VERSION_MAJOR EQUAL 0)
    set(RTFW_PACKAGE_VERSION_COMPATIBILITY SameMinorVersion)
else()
    set(RTFW_PACKAGE_VERSION_COMPATIBILITY SameMajorVersion)
endif()

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/rtfwConfigVersion.cmake
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY "${RTFW_PACKAGE_VERSION_COMPATIBILITY}"
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/rtfwConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/rtfwConfigVersion.cmake
    DESTINATION ${RTFW_INSTALL_CMAKEDIR}
)

install(
    FILES
        LICENSE
        VERSION.txt
        CHANGELOG.md
        SECURITY.md
        docs/runtime_profiles.md
        docs/cuda_physics.md
        docs/cuda_pipeline.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw
)

# Source-only example: no default SDK export or vendor dependency is added.
install(FILES
    samples/cuda_physics/CMakeLists.txt
    samples/cuda_physics/real.cmake
    samples/cuda_physics/model.hpp
    samples/cuda_physics/scenario.hpp
    samples/cuda_physics/simulated_driver.hpp
    samples/cuda_physics/cli.hpp
    samples/cuda_physics/main.cpp
    samples/cuda_physics/real.cpp
    samples/cuda_physics/particle.cu
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/cuda_physics)
install(FILES
    samples/cuda_physics/pipeline/scenario.hpp
    samples/cuda_physics/pipeline/simulated_driver.hpp
    samples/cuda_physics/pipeline/cli.hpp
    samples/cuda_physics/pipeline/main.cpp
    samples/cuda_physics/pipeline/real.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/cuda_physics/pipeline)


install(
    FILES
        tools/autotune/config.schema.json
        configs/default.json
        configs/default_fast.json
        configs/default_safe.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/profiles
)

install(
    FILES
        abi/rtfw_c_abi_v8.exports
        abi/rtfw_c_abi_v8.sha256
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/abi
)

install(
    FILES
        docs/portable_support_matrix.json
        docs/release_policy.md
        docs/cuda_support_matrix.json
        docs/xdma_support_matrix.json
        release/rtfw-release-contract.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/release
)

install(
    EXPORT rtfwTargets
    NAMESPACE rtfw::
    DESTINATION ${RTFW_INSTALL_CMAKEDIR}
)

if (RTFW_ENABLE_CUDA)
  install(
      EXPORT rtfwCudaTargets
      FILE rtfwCudaTargets.cmake
      NAMESPACE rtfw::
      DESTINATION ${RTFW_INSTALL_CMAKEDIR})
endif()

if (RTFW_ENABLE_XDMA)
  install(
      EXPORT rtfwXdmaTargets
      FILE rtfwXdmaTargets.cmake
      NAMESPACE rtfw::
      DESTINATION ${RTFW_INSTALL_CMAKEDIR})
endif()

if (RTFW_INSTALL_EXPERIMENTAL)
  install(
      EXPORT rtfwExperimentalTargets
      FILE rtfwExperimentalTargets.cmake
      NAMESPACE rtfw::
      DESTINATION ${RTFW_INSTALL_CMAKEDIR})
endif()

if (RTFW_TOP_LEVEL)
  set(CPACK_PACKAGE_NAME "rtfw")
  set(CPACK_PACKAGE_VENDOR "RTFW contributors")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Bounded-resource C++20 simulation runtime")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(RTFW_PACKAGE_TOOLCHAIN
      "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
  string(REGEX REPLACE "[^A-Za-z0-9._-]" "_"
      RTFW_PACKAGE_TOOLCHAIN "${RTFW_PACKAGE_TOOLCHAIN}")
  set(CPACK_PACKAGE_FILE_NAME
      "rtfw-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${RTFW_PACKAGE_TOOLCHAIN}")
  set(CPACK_PACKAGE_CHECKSUM SHA256)
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
  set(CPACK_VERBATIM_VARIABLES YES)
  if (WIN32)
      set(CPACK_GENERATOR ZIP)
  else()
      set(CPACK_GENERATOR TGZ)
  endif()
  include(CPack)
endif()

install(FILES
    samples/cuda_physics/lifetime/protocol.hpp
    samples/cuda_physics/lifetime/conformance.hpp
    samples/cuda_physics/lifetime/main.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/cuda_physics/lifetime)
install(FILES docs/cuda_lifetime.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

install(FILES
    samples/cuda_physics/benchmark/provider.hpp
    samples/cuda_physics/benchmark/provider.cpp
    samples/cuda_physics/benchmark/main.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/cuda_physics/benchmark)
install(FILES docs/cuda_benchmarking.md docs/cuda_capabilities.json docs/cuda_capabilities.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

# Source-only onboarding kit; no SDK export or public header is added.
install(FILES samples/hello_runtime/main.cpp samples/hello_runtime/CMakeLists.txt
    samples/hello_runtime/check_output.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/hello_runtime)
install(FILES docs/getting_started.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

install(FILES samples/typed_runtime/main.cpp samples/typed_runtime/CMakeLists.txt
    samples/typed_runtime/check_output.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/typed_runtime)
install(FILES docs/typed_runtime.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

# Source-only authoring kit: no SDK header or exported-target additions.
install(FILES samples/backend_authoring/main.cpp samples/backend_authoring/CMakeLists.txt
    samples/backend_authoring/check_output.cmake samples/backend_authoring/backend.hpp
    samples/backend_authoring/profile.hpp samples/backend_authoring/conformance.hpp
    samples/backend_authoring/runtime_example.hpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/backend_authoring)
install(FILES docs/backend_authoring.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

# Optional source reference, never a mandatory Runtime dependency or SDK target.
install(FILES samples/cil_shared_memory/CMakeLists.txt
    samples/cil_shared_memory/channel.hpp samples/cil_shared_memory/common.hpp
    samples/cil_shared_memory/controller.cpp samples/cil_shared_memory/mapping.hpp
    samples/cil_shared_memory/plant.cpp samples/cil_shared_memory/plant_runtime.hpp
    samples/cil_shared_memory/runner.py samples/cil_shared_memory/wire.hpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/cil_shared_memory)
install(FILES docs/cil_shared_memory.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

# Independent host reference is installed source, not an exported SDK dependency.
install(FILES samples/host_adapter/CMakeLists.txt samples/host_adapter/main.cpp
    samples/host_adapter/host.hpp samples/host_adapter/jobs.hpp
    samples/host_adapter/memory.hpp samples/host_adapter/check_output.cmake
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/host_adapter)
install(FILES docs/host_adapter.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

# M25-06: checked-in offline docs need no generator at consumer build/install time.
install(DIRECTORY docs/sdk/generated/ DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/manual)
install(FILES samples/recipes/CMakeLists.txt samples/recipes/transcripts.json
    samples/recipes/run_transcripts.py DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/recipes)
install(FILES tests/mixed_rate_conformance.hpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/recipes)
install(FILES tests/package_consumer/c_consumer.c tests/package_consumer/cpp_consumer.cpp
    tests/package_consumer/profile_consumer.cpp tests/package_consumer/mixed_rate_consumer.cpp
    tests/package_consumer/live_control_consumer.cpp tests/package_consumer/live_control_replay_consumer.cpp
    tests/package_consumer/cuda_consumer.cpp tests/package_consumer/xdma_consumer.cpp
    tests/package_consumer/benchmark_consumer.cpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/recipes/package_consumer)

# M26-02: complete optional public-SDK source kit, preserving SDK exports.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/golden_system/provenance.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/golden-provenance.json"
    RESULT_VARIABLE golden_provenance_result)
if(NOT golden_provenance_result EQUAL 0)
    message(FATAL_ERROR "Golden source provenance generation failed")
endif()
install(FILES
    samples/golden_system/CMakeLists.txt
    samples/golden_system/README.md
    samples/golden_system/artifacts.py
    samples/golden_system/channel.hpp
    samples/golden_system/common.hpp
    samples/golden_system/contract.json
    samples/golden_system/controller.cpp
    samples/golden_system/controls.hpp
    samples/golden_system/fixed.hpp
    samples/golden_system/jobs.hpp
    samples/golden_system/main.cpp
    samples/golden_system/mapping.hpp
    samples/golden_system/memory.hpp
    samples/golden_system/model.hpp
    samples/golden_system/oracle.hpp
    samples/golden_system/peer.hpp
    samples/golden_system/physics.hpp
    samples/golden_system/provenance.py
    samples/golden_system/replay.hpp
    samples/golden_system/run.py
    samples/golden_system/session.hpp
    samples/golden_system/telemetry.hpp
    samples/golden_system/wire.hpp
    samples/golden_system/world.hpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_system)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/golden-provenance.json"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_system RENAME provenance.json)
install(FILES docs/golden_system.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/golden_cuda/provenance.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/golden-cuda-provenance.json"
    RESULT_VARIABLE golden_cuda_provenance_result)
if(NOT golden_cuda_provenance_result EQUAL 0)
    message(FATAL_ERROR "Golden CUDA source provenance generation failed")
endif()
install(FILES
    samples/golden_cuda/CMakeLists.txt
    samples/golden_cuda/README.md
    samples/golden_cuda/artifacts.py
    samples/golden_cuda/main.cpp
    samples/golden_cuda/native_launcher.cpp
    samples/golden_cuda/owner.hpp
    samples/golden_cuda/physics.cu
    samples/golden_cuda/physics.hpp
    samples/golden_cuda/provenance.py
    samples/golden_cuda/real.cmake
    samples/golden_cuda/real.cpp
    samples/golden_cuda/replay.hpp
    samples/golden_cuda/resources.hpp
    samples/golden_cuda/run.py
    samples/golden_cuda/runner.hpp
    samples/golden_cuda/simulated_driver.hpp
    samples/golden_cuda/telemetry.hpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_cuda)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/golden-cuda-provenance.json"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_cuda RENAME provenance.json)
install(FILES docs/golden_cuda.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/samples/golden_xdma/provenance.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/golden-xdma-provenance.json"
    RESULT_VARIABLE golden_xdma_provenance_result)
if(NOT golden_xdma_provenance_result EQUAL 0)
    message(FATAL_ERROR "Golden XDMA source provenance generation failed")
endif()
install(FILES
    samples/golden_xdma/CMakeLists.txt
    samples/golden_xdma/README.md
    samples/golden_xdma/artifacts.py
    samples/golden_xdma/codec.hpp
    samples/golden_xdma/io.hpp
    samples/golden_xdma/main.cpp
    samples/golden_xdma/oracle.hpp
    samples/golden_xdma/owner.hpp
    samples/golden_xdma/provenance.py
    samples/golden_xdma/replay.hpp
    samples/golden_xdma/run.py
    samples/golden_xdma/runner.hpp
    samples/golden_xdma/session.hpp
    samples/golden_xdma/simulated_driver.hpp
    samples/golden_xdma/state.py
    samples/golden_xdma/telemetry.hpp
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_xdma)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/golden-xdma-provenance.json"
    DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_xdma RENAME provenance.json)
install(FILES docs/golden_xdma.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)

# Optional source-only showcase; default Runtime installation stays unchanged.
if(RTFW_BUILD_BENCHMARKS)
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/samples/golden_showcase/provenance.py"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/golden-showcase-provenance.json"
        RESULT_VARIABLE golden_showcase_provenance_result)
    if(NOT golden_showcase_provenance_result EQUAL 0)
        message(FATAL_ERROR "Golden showcase source provenance generation failed")
    endif()
    install(FILES
        samples/golden_showcase/CMakeLists.txt
        samples/golden_showcase/README.md
        samples/golden_showcase/artifacts.py
        samples/golden_showcase/experiment_main.cpp
        samples/golden_showcase/experiments.cpp
        samples/golden_showcase/experiments.hpp
        samples/golden_showcase/main.cpp
        samples/golden_showcase/probes.hpp
        samples/golden_showcase/provenance.py
        samples/golden_showcase/provider.cpp
        samples/golden_showcase/provider.hpp
        samples/golden_showcase/replay.hpp
        samples/golden_showcase/replay_gate.hpp
        samples/golden_showcase/report.py
        samples/golden_showcase/run.py
        samples/golden_showcase/session.hpp
        samples/golden_showcase/state.py
        samples/golden_showcase/telemetry_loss.cpp
        samples/golden_showcase/test.py
        samples/golden_showcase/variants.cpp
        samples/golden_showcase/variants.hpp
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_showcase)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/golden-showcase-provenance.json"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw/examples/golden_showcase RENAME provenance.json)
    install(FILES docs/golden_showcase.md DESTINATION ${CMAKE_INSTALL_DATADIR}/rtfw)
endif()
