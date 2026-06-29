# url-parser

A small C++20 parser that splits a URL path and query string into their
components. Extracted from [Xapiand](https://github.com/Kronuz/Xapiand).

## What it is

Three pieces, all driven off a single decoded buffer:

- **`urldecode()`** turns a percent-encoded byte buffer into a plain string:
  `%NN` escapes are decoded, `+` becomes a space, and the special separators
  (`&`, `;`, `=`) are remappable so the parsers can re-encode their own
  delimiters before scanning.
- **`QueryParser`** walks a `key=value&key2=value2` query string. You `init()`
  it once, then call `next("name")` to seek a key (returns `0` on a hit, `-1`
  on a miss) and `get()` to read the decoded value. A bare flag with no `=` is
  found with an empty value.
- **`PathParser`** takes a path and splits it into an id (the last segment), the
  leading path, an optional `:command` suffix, an optional `{...}` field
  selector, and an optional `host@` prefix. It runs the path through a
  `normalize_path` pass first, so `a/../b` and `x//y` are collapsed before
  parsing.

It is what Xapiand uses to turn an incoming HTTP request line into the index,
document id, command, and query parameters it needs to dispatch.

## Install

This is not header-only. It ships a compiled translation unit, `url_parser.cc`,
plus the header `url_parser.h`. You build and link the `.cc`; the header alone
only declares `urldecode` and the `QueryParser` / `PathParser` classes. It
requires C++20.

It has one dependency:
[char-classify](https://github.com/Kronuz/char-classify), the sibling header
that provides `chars::hexdec` (the percent-escape hex decoder). CMake pulls it
in for you.

With CMake `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
  url_parser
  GIT_REPOSITORY https://github.com/Kronuz/url-parser.git
  GIT_TAG        main
)
FetchContent_MakeAvailable(url_parser)

target_link_libraries(your_target PRIVATE url_parser::url_parser)
```

`CMakeLists.txt` requests `cxx_std_20` `PUBLIC` and links `char-classify`
`PUBLIC`, so `"chars.hh"` resolves on the include path with no extra wiring on
your side. Then:

```cpp
#include "url_parser.h"
```

The header keeps its original filename, so a codebase that already
`#include "url_parser.h"` just needs this repo on its include path.

## Usage

```cpp
#include "url_parser.h"

// Decode a percent-encoded buffer: %NN escapes and '+' -> space.
urldecode(std::string_view("a%20b+c"));       // "a b c"
urldecode(std::string_view("hello%2Fworld")); // "hello/world"

// Read a query parameter.
QueryParser qp;
qp.init("foo=1&bar=hello%20world");
if (qp.next("bar", std::strlen("bar")) == 0) {
    qp.get();                                 // "hello world" (decoded)
}

// Split a path. The last segment is the id, the rest is the path.
PathParser pp;
pp.init("type/id:count");
pp.get_id();                                  // "id"
pp.get_cmd();                                 // ":count"
pp.next();                                    // State::PTH
pp.get_pth();                                 // "type"

// normalize_path runs first, so ".." and "//" are folded before parsing.
pp.init("a/../b");                            // id "b", pth "a"
pp.init("x//y");                              // id "y", pth "x"
```

## API reference

```cpp
std::string urldecode(const void* p, size_t size,
                      char plus = ' ', char amp = '&', char colon = ';',
                      char eq = '=', char encoded = '\0');
std::string urldecode(std::string_view, /* same separators */);
```

The core decoder, plus a `std::string_view` and a string-literal overload that
forward to it. The separator arguments let a caller remap `+`, `&`, `;`, `=`
to other bytes (the parsers use this to mark their own delimiters); `encoded`,
when non-zero, replaces a percent-decoded byte unless it is one of `{ , }`.

```cpp
class QueryParser {
    void clear() noexcept;
    void rewind() noexcept;
    int  init(std::string_view q);          // load the query string
    int  next(const char* name, size_t n);  // 0 on hit, -1 on miss
    std::string_view get();                 // decoded value of the last hit
};
```

```cpp
class PathParser {
    enum class State : uint8_t { /* PTH, HST, CMD, SLC, END, ... */ };

    void  clear() noexcept;
    void  rewind() noexcept;
    State init(std::string_view p);  // backward scan: id, command, selector
    State next();                    // forward scan: path, host
    bool  has_pth() noexcept;

    std::string_view get_pth();      // leading path
    std::string_view get_hst();      // host (the "host@" prefix)
    std::string_view get_id();       // last segment
    std::string_view get_slc();      // {...} field selector
    std::string_view get_cmd();      // ":command" suffix
};
```

`init()` runs a single backward pass to peel off the id, the `:command`, and
any `{...}` selector. `next()` then runs forward over what is left, yielding the
path (`State::PTH`) or, when a `host@` prefix is present, the host
(`State::HST`), and returns `State::END` when there is nothing more.

## Build & test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The first configure fetches `char-classify` over the network. The test
exercises `urldecode` (percent escapes, `+` -> space), `QueryParser`
(hit/miss/bare flag and decoded values), and `PathParser` (the id / path /
command / host split and the `normalize_path` collapsing of `..` and `//`). It
prints `all url_parser tests passed` and exits 0.

## Examples

[`examples/demo.cc`](examples/demo.cc) is a runnable tour. A top-level CMake
build produces it next to the test:

```sh
cmake -B build && cmake --build build && ./build/url_parser_demo
```

It decodes a couple of buffers, reads several keys out of a query string, and
splits a handful of representative paths, printing the components for each.

## Provenance

Extracted from [Xapiand](https://github.com/Kronuz/Xapiand). `url_parser.h` was
copied verbatim. `url_parser.cc` got two decoupling edits so it could stand
alone: the `normalize_path` helper it used from `"fs.hh"` was inlined (it is
pure string manipulation, no filesystem access), and Xapiand's `"log.h"` /
`L_*` trace logging was removed (a standalone parser does not log). Its one
remaining local include, `"chars.hh"`, is wired to the standalone
[char-classify](https://github.com/Kronuz/char-classify) library through CMake.
See [ARCHITECTURE.md](ARCHITECTURE.md) for the design and
[AGENTS.md](AGENTS.md) for the repo map and invariants.

## License

MIT, Copyright (c) 2015-2019 Dubalu LLC. See [LICENSE](LICENSE).
