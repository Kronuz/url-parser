cmake_minimum_required(VERSION 3.14)
project(url_parser LANGUAGES CXX)

include(FetchContent)

# --- Dependency: char-classify ---
# The library has no dependency of its own (urldecode decodes hex through a local
# table). test/urldecode_old.h, the reference decoder of the differential test, the
# fuzz target and the benchmark, includes "chars.hh" for chars::hexdec; that header
# lives in the sibling char-classify library. It is linked PUBLIC for its include path
# and pinned to a commit.
FetchContent_Declare(
  char_classify
  GIT_REPOSITORY https://github.com/Kronuz/char-classify.git
  GIT_TAG        57613ed349bb4fa273806b78341a53f676089505
)
FetchContent_MakeAvailable(char_classify)

# --- Library target ---
# url_parser is not header-only: url_parser.cc carries the urldecode loop and
# the two parser state machines, so it must be compiled and linked. The header
# alone (url_parser.h) only declares urldecode and the QueryParser / PathParser
# classes.
add_library(url_parser url_parser.cc)
target_include_directories(url_parser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(url_parser PUBLIC cxx_std_20)

# Link char-classify so "chars.hh" is on the include path. PUBLIC because
# url_parser.cc uses it; consumers don't need chars.hh themselves, but linking
# it PUBLIC keeps the include path consistent and harmless.
target_link_libraries(url_parser PUBLIC char_classify::char_classify)

# Alias so consumers can link `url_parser::url_parser` whether they add this via
# add_subdirectory or FetchContent.
add_library(url_parser::url_parser ALIAS url_parser)

# Only build the test and demo when this repo is the top-level project, so
# consumers (e.g. via FetchContent) don't build them.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  enable_testing()
  add_executable(url_parser_test test/test.cc)
  target_link_libraries(url_parser_test PRIVATE url_parser)
  add_test(NAME url_parser COMMAND url_parser_test)

  # Differential test against the frozen original urldecode. It #includes
  # url_parser.cc to reach the private buffers, so it does not link the library.
  add_executable(url_parser_diff test/urldecode_diff.cc)
  target_include_directories(url_parser_diff PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test")
  target_link_libraries(url_parser_diff PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_diff PRIVATE cxx_std_20)
  add_test(NAME url_parser_diff COMMAND url_parser_diff)

  # The decoder against a specification written from the documented semantics, independent of the
  # reference above: exhaustive short strings, every length to 200, random and very large inputs.
  add_executable(url_parser_spec test/urldecode_spec.cc)
  target_include_directories(url_parser_spec PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(url_parser_spec PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_spec PRIVATE cxx_std_20)
  add_test(NAME url_parser_spec COMMAND url_parser_spec)

  # normalize_path (the helper PathParser::init runs first) against its frozen previous loop and
  # std::filesystem::path::lexically_normal: the empty and null path, the first component of a relative path.
  add_executable(url_parser_normalize test/normalize_path_test.cc)
  target_include_directories(url_parser_normalize PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(url_parser_normalize PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_normalize PRIVATE cxx_std_20)
  add_test(NAME url_parser_normalize COMMAND url_parser_normalize)

  # QueryParser::next and PathParser::next against the byte loops they replace, on buffers with
  # long fields and delimiters at every offset of the vector steps.
  add_executable(url_parser_next_model test/next_model.cc)
  target_include_directories(url_parser_next_model PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(url_parser_next_model PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_next_model PRIVATE cxx_std_20)
  add_test(NAME url_parser_next_model COMMAND url_parser_next_model)

  # The same test with the portable SWAR scanner forced (the default scanner is NEON or SSE2).
  add_executable(url_parser_diff_swar test/urldecode_diff.cc)
  target_include_directories(url_parser_diff_swar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test")
  target_link_libraries(url_parser_diff_swar PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_diff_swar PRIVATE cxx_std_20)
  target_compile_definitions(url_parser_diff_swar PRIVATE URL_PARSER_NO_SIMD=1)
  add_test(NAME url_parser_diff_swar COMMAND url_parser_diff_swar)

  # On x86-64 the same test with the AVX2 skip left out (the default build uses it where the CPU has it).
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_executable(url_parser_diff_noavx2 test/urldecode_diff.cc)
    target_include_directories(url_parser_diff_noavx2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_link_libraries(url_parser_diff_noavx2 PRIVATE char_classify::char_classify)
    target_compile_features(url_parser_diff_noavx2 PRIVATE cxx_std_20)
    target_compile_definitions(url_parser_diff_noavx2 PRIVATE URL_PARSER_NO_AVX2=1)
    add_test(NAME url_parser_diff_noavx2 COMMAND url_parser_diff_noavx2)
  endif()

  # Timing guards: short input costs no more than the reference decoder, and the scan takes
  # several vectors per step. Each ratio compares two things timed in the same process; the
  # test is skipped (status 77) in unoptimized and sanitized builds, and runs alone.
  add_executable(url_parser_perf test/perf_guard.cc)
  target_include_directories(url_parser_perf PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test")
  target_link_libraries(url_parser_perf PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_perf PRIVATE cxx_std_20)
  add_test(NAME url_parser_perf COMMAND url_parser_perf)
  set_tests_properties(url_parser_perf PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)

  # tools/mutants.py: each mutant's anchor in url_parser.cc still occurs exactly once. Running the mutants
  # (`python3 tools/mutants.py`) rebuilds the suite for each of them and is not a test.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME url_parser_mutants_anchors COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/mutants.py --check)
  endif()

  # libFuzzer target (Clang): cmake -DURL_PARSER_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
  option(URL_PARSER_FUZZ "Build the libFuzzer target" OFF)
  if(URL_PARSER_FUZZ)
    add_executable(url_parser_fuzz fuzz/fuzz_parse.cc url_parser.cc)
    target_include_directories(url_parser_fuzz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test")
    target_link_libraries(url_parser_fuzz PRIVATE char_classify::char_classify)
    target_compile_features(url_parser_fuzz PRIVATE cxx_std_20)
    target_compile_options(url_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(url_parser_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  endif()

  # Benchmark vs the frozen original, a LUT decoder and ada::unicode::percent_decode:
  #   cmake -B build -DURL_PARSER_BENCH=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build --target url_parser_bench
  # ada is fetched only here; the library target never depends on it. -DURL_PARSER_BENCH_ADA=OFF skips it.
  option(URL_PARSER_BENCH "Build the urldecode benchmark" OFF)
  if(URL_PARSER_BENCH)
    add_executable(url_parser_bench bench/bench_url.cc)
    target_include_directories(url_parser_bench PRIVATE bench test)
    target_link_libraries(url_parser_bench PRIVATE url_parser)
    option(URL_PARSER_BENCH_ADA "Compare against ada percent_decode" ON)
    if(URL_PARSER_BENCH_ADA)
      FetchContent_Declare(ada GIT_REPOSITORY https://github.com/ada-url/ada.git
                               GIT_TAG 3306b15d89939d0374338e61ed362fcc5380a991)
      cmake_policy(SET CMP0169 OLD)  # populate ada's sources only, not its build
      FetchContent_GetProperties(ada)
      if(NOT ada_POPULATED)
        FetchContent_Populate(ada)
      endif()
      add_library(ada_bench_obj OBJECT "${ada_SOURCE_DIR}/src/ada.cpp")
      target_include_directories(ada_bench_obj PUBLIC "${ada_SOURCE_DIR}/include")
      target_compile_features(ada_bench_obj PUBLIC cxx_std_20)
      target_link_libraries(url_parser_bench PRIVATE ada_bench_obj)
      target_compile_definitions(url_parser_bench PRIVATE HAVE_ADA=1)
    endif()
  endif()

  # The same-run sweep of urldecode, QueryParser and PathParser over sizes of 4 to 4096 bytes, beside the
  # original library (4424e25) and the one before the small-input work (9ce4c23), and optionally another
  # commit: cmake -B build -DURL_PARSER_SWEEP=ON [-DURL_PARSER_SWEEP_BASE=<commit>] -DCMAKE_BUILD_TYPE=Release
  # The earlier commits are fetched here only and built into the one binary under other names
  # (bench/sweep/*_api.h and *_impl.cc); the library target never depends on them.
  option(URL_PARSER_SWEEP "Build the sweep against earlier commits" OFF)
  set(URL_PARSER_SWEEP_BASE "" CACHE STRING "Another commit of this repository for the sweep to compare with")
  if(URL_PARSER_SWEEP)
    if(POLICY CMP0169)
      cmake_policy(SET CMP0169 OLD)  # populate the sources only, not a build
    endif()
    # Fetches commit `commit` of this repository into <build>/sweep-src/<variant>.
    function(url_parser_sweep_fetch variant commit)
      FetchContent_Declare(sweep_${variant} GIT_REPOSITORY https://github.com/Kronuz/url-parser.git GIT_TAG ${commit}
                           SOURCE_DIR "${CMAKE_BINARY_DIR}/sweep-src/${variant}")
      FetchContent_GetProperties(sweep_${variant})
      if(NOT sweep_${variant}_POPULATED)
        FetchContent_Populate(sweep_${variant})
      endif()
      # The headers of two commits can be the same file, and GCC reads a `#pragma once` file once by its
      # content and time, so the second variant would lose its declarations. Each one is read once, under
      # its own names, by its api header.
      set(header_path "${CMAKE_BINARY_DIR}/sweep-src/${variant}/url_parser.h")
      file(READ "${header_path}" header)
      if(header MATCHES "#pragma once")
        string(REPLACE "#pragma once" "" header "${header}")
        file(WRITE "${header_path}" "${header}")
      endif()
    endfunction()
    url_parser_sweep_fetch(orig 4424e25c53c54ae5697ddae2dd643885d921b4e2)
    url_parser_sweep_fetch(prev 9ce4c230cb324470e0205d233c0ad088d29fc694)
    set(sweep_sources bench/sweep.cc bench/sweep/orig_impl.cc bench/sweep/prev_impl.cc)
    if(URL_PARSER_SWEEP_BASE)
      url_parser_sweep_fetch(base ${URL_PARSER_SWEEP_BASE})
      list(APPEND sweep_sources bench/sweep/base_impl.cc)
    endif()
    add_executable(url_parser_sweep ${sweep_sources})
    target_include_directories(url_parser_sweep PRIVATE bench/sweep test "${CMAKE_BINARY_DIR}/sweep-src")
    target_link_libraries(url_parser_sweep PRIVATE url_parser)
    if(URL_PARSER_SWEEP_BASE)
      target_compile_definitions(url_parser_sweep PRIVATE URL_PARSER_SWEEP_BASE_SET=1)
    endif()
    # The test runs a few rows once, so a build that cannot run the sweep fails; it times nothing.
    add_test(NAME url_parser_sweep COMMAND url_parser_sweep "n=8 " 1 0 16 0 op)
  endif()

  # A runnable tour of the library (not a test): build & run ./build/url_parser_demo
  add_executable(url_parser_demo examples/demo.cc)
  target_link_libraries(url_parser_demo PRIVATE url_parser)
endif()
