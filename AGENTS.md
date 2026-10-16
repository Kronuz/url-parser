# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
url_parser.h                 Declares urldecode() and the QueryParser / PathParser classes. Header. Verbatim from Xapiand.
url_parser.cc                urldecode, the two parsers, and the inlined normalize_path. Includes "chars.hh" for chars::hexdec. Decoupled from Xapiand (see below).
test/test.cc                 Runnable smoke test: urldecode, QueryParser, PathParser, and the normalize_path collapsing.
examples/demo.cc             A runnable tour (not a test).
CMakeLists.txt               STATIC library `url_parser` (+ alias url_parser::url_parser); FetchContent char-classify; CTest test `url_parser`.
LICENSE                      MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                    What it is, install, quick start.
docs/API.md                  The API reference and caveats.
docs/url-parser.d2           The README diagram's source (docs/url-parser.svg is rendered from it).
ARCHITECTURE.md              The decoded-buffer model, the backward/forward path passes, normalize_path.
```

This is not header-only. `url_parser.cc` must be compiled and linked; the header
only declares the API. The CMake target is a `STATIC` library that links
`char-classify` so `"chars.hh"` resolves.

## Build and run the test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

The first configure fetches `char-classify` over the network (FetchContent,
`GIT_TAG main`). Expected output ends with `all url_parser tests passed`, exit 0.
The test target is `url_parser_test`; the registered CTest name is `url_parser`.
The test and demo are only added when this repo is the top-level project
(CMakeLists.txt), so consumers vendoring it via `FetchContent` won't build them.

## Dependency

`url_parser.cc` includes `"chars.hh"` for one symbol, `chars::hexdec` (reads the
two hex digits of a `%NN` escape through a `const char**`). That header lives in
the sibling [char-classify](https://github.com/Kronuz/char-classify) library,
pulled in by CMake and linked `PUBLIC` so the include path resolves. We track
its tip with `GIT_TAG main`, like the rest of the family. This is the library's
only dependency.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand). `url_parser.h` was copied
verbatim. `url_parser.cc` carries two decoupling edits, and they are the only
deltas from upstream:

- **`normalize_path` inlined.** Upstream `url_parser.cc` did
  `#include "fs.hh"` for the single call `normalize_path(p, false, true)` in
  `PathParser::init`. That function is pure string manipulation (no filesystem
  access), so both its overloads were copied verbatim from Xapiand's `src/fs.cc`
  into an anonymous namespace at the top of this file, and the `"fs.hh"` include
  was dropped. Keep the copyright attribution comment on the copied helper.
- **Trace logging removed.** Upstream included `"log.h"`, defined
  `L_URL_PARSER`, and had a handful of `L_URL_PARSER(...)` trace calls in the
  parse path (including one `repr(path)` dump). A standalone parser does not log,
  so the include, the macro, and every `L_*` call were removed. The blocks they
  sat in (the two big state-name-dumping lambdas in `init` / `next`) were dropped
  cleanly, leaving the `switch (cn)` that followed them. There is no remaining
  `L_*` / `log.h` / `opts` / `config` / `repr` reference.

Any future change should be reconcilable with upstream as a plain edit on top of
those two deltas.

## Conventions

- **C++20.** The target requests `cxx_std_20` `PUBLIC` to stay uniform with the
  sibling libraries. Don't drop the target below it.
- **Filenames are stable.** The header and source keep their original Xapiand
  names (`url_parser.h`, `url_parser.cc`) so a consumer that already
  `#include`s the header just needs this repo on the include path. Don't rename
  them.
- Tabs for indentation, double quotes in code, no em dashes in prose.
- MIT-licensed; keep the copyright header (Copyright (c) 2015-2019 Dubalu LLC) on
  source files.

## Load-bearing invariants

- **Two decoded buffers in lockstep.** Each parser keeps a delimiter-remapped
  scan buffer and a normally-decoded value buffer of the same length. Offsets
  found in the scan buffer are used to index the value buffer when returning a
  `string_view`. If you change how one buffer is decoded, the other must change
  the same way or `get_*()` will return garbage.
- **PathParser scans backward then forward.** `init()` goes right-to-left to peel
  the id, `:command`, and `{...}` selector off the tail; `next()` goes
  left-to-right for the path and host. The forward pass clamps its end to the
  offsets the backward pass found so they never overlap. Don't merge them into
  one pass; the two-pass split is what makes every trailing piece optional.
- **`normalize_path` runs before parsing.** `PathParser::init` calls
  `normalize_path(p, false, true)` first, so the parser only ever sees a path
  with `..` folded, `//` collapsed, and the trailing slash kept. The inlined copy
  must stay byte-for-byte equivalent to upstream's `src/fs.cc`; it is pure string
  work, so don't add any filesystem access to it.

## Traps

- **The delimiter sentinels are control bytes, not the literal characters.** The
  parsers decode with `=` mapped to `\1`, `&`/`;` mapped to `\0`, etc., then
  switch on those control bytes. If you read the scan code expecting literal
  `'='` / `'&'`, you'll misread it. The literal value is recovered from the
  second (normally decoded) buffer.
- **`get_*()` returns an empty `string_view` when the component is absent.** A
  missing id / host / command / selector is reported as an empty view (offset
  `nullptr`), not as a parse error. Callers distinguish "absent" via the
  returned `State` and `has_pth()`.
- **`next(name, n)` takes an explicit length.** The `QueryParser::next` overload
  used in code takes `(const char*, size_t)`; there is also a template overload
  for string literals that subtracts the trailing NUL. Pass the right length or
  the key match will be off by one.

## How to extend

- **Always extend the smoke test.** `test/test.cc` is the only executable check.
  Add a case for any new behavior and assert the exact `string_view` the parser
  returns.
