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
  GIT_TAG        main
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

  # A runnable tour of the library (not a test): build & run ./build/url_parser_demo
  add_executable(url_parser_demo examples/demo.cc)
  target_link_libraries(url_parser_demo PRIVATE url_parser)
endif()
