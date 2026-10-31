cmake_minimum_required(VERSION 3.14)
project(url_parser LANGUAGES CXX)

include(FetchContent)

# --- Dependency: char-classify ---
# url_parser.cc includes "chars.hh" for chars::hexdec (the percent-escape hex
# decoder used by urldecode). That header lives in the sibling char-classify
# library, pulled in here so the include resolves. We track its tip with
# GIT_TAG main, like the rest of the family.
FetchContent_Declare(
  char_classify
  GIT_REPOSITORY https://github.com/Kronuz/char-classify.git
  GIT_TAG        a8904965dc904690ddd6875ab7acbfd271d97832
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

  # The same test with the portable SWAR scanner forced (the default scanner is NEON or SSE2).
  add_executable(url_parser_diff_swar test/urldecode_diff.cc)
  target_include_directories(url_parser_diff_swar PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/test")
  target_link_libraries(url_parser_diff_swar PRIVATE char_classify::char_classify)
  target_compile_features(url_parser_diff_swar PRIVATE cxx_std_20)
  target_compile_definitions(url_parser_diff_swar PRIVATE URL_PARSER_NO_SIMD=1)
  add_test(NAME url_parser_diff_swar COMMAND url_parser_diff_swar)

  # libFuzzer target (Clang): cmake -DURL_PARSER_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
  option(URL_PARSER_FUZZ "Build the libFuzzer target" OFF)
  if(URL_PARSER_FUZZ)
    add_executable(url_parser_fuzz fuzz/fuzz_parse.cc url_parser.cc)
    target_include_directories(url_parser_fuzz PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
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

  # A runnable tour of the library (not a test): build & run ./build/url_parser_demo
  add_executable(url_parser_demo examples/demo.cc)
  target_link_libraries(url_parser_demo PRIVATE url_parser)
endif()
