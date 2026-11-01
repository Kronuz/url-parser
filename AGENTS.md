# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps that are
easy to fall into.

## Repo map

```
url_parser.h                 Declares urldecode() and the QueryParser / PathParser classes. Header. Verbatim from Xapiand.
url_parser.cc                urldecode (SIMD-scanned, single pass), the two parsers, and the inlined normalize_path. Decoupled from Xapiand (see below).
test/urldecode_diff.cc       Differential test (CTest `url_parser_diff`, `url_parser_diff_swar` with the SWAR scanner, and on x86-64 `url_parser_diff_noavx2` without the AVX2 skip) against the reference test/urldecode_old.h.
test/urldecode_spec.cc       Specification test (CTest `url_parser_spec`): urldecode and both parsers' buffers against a decoder written from the documented semantics, not from the implementation: exhaustive short strings, every length to 200, random and 64 KB to 1 MB inputs.
test/normalize_path_test.cc  CTest `url_parser_normalize`: normalize_path against fs's loop, a model and a table of `os.path.normpath`'s output (the current directory is `.`), and the original loop frozen: every string of `a`, `b`, `.`, `/` to 8 bytes, the empty and null path, and what PathParser makes of them.
test/next_model.cc           CTest `url_parser_next_model`: QueryParser::next and PathParser::next against the byte loops they replace (kept in the test), on buffers with long fields and delimiters at every offset of the vector steps.
test/perf_guard.cc           Timing guards (CTest `url_parser_perf`): short input is no slower than the reference decoder, the scan takes several vectors per step, and QueryParser::next crosses a long field with vectors. Skipped in unoptimized and sanitized builds.
fuzz/fuzz_parse.cc           libFuzzer target (-DURL_PARSER_FUZZ=ON): urldecode with fuzzed substitutes against the reference decoder, and both parsers' buffers.
fuzz/url.dict               libFuzzer dictionary of the bytes and sequences that matter (`url_parser_fuzz -dict=fuzz/url.dict`).
bench/bench_url.cc           Benchmark vs the reference decoder, a LUT decoder and ada (cmake -DURL_PARSER_BENCH=ON): urldecode from 4 bytes to 64 KB, QueryParser and PathParser.
bench/sweep.cc, bench/sweep/ The same-run sweep over sizes of 4 to 4096 bytes beside the original library (4424e25), the one before the small-input work (9ce4c23) and an optional commit, each fetched and built under other names into one binary (cmake -DURL_PARSER_SWEEP=ON).
tools/mutants.py             The 14 decoder, 5 scan and 9 normalize_path mutants of url_parser.cc, applied to a scratch copy, rebuilt and run against the suite; CTest `url_parser_mutants_anchors` keeps their anchors matching.
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

The first configure fetches `char-classify` over the network (FetchContent, at the
commit `CMakeLists.txt` pins). Expected output ends with `all url_parser tests passed`, exit 0.
The test target is `url_parser_test`; the registered CTest name is `url_parser`.
The test and demo are only added when this repo is the top-level project
(CMakeLists.txt), so consumers vendoring it via `FetchContent` won't build them.

## Verification and benchmarks

The method shared by the Kronuz libraries is in
[docs/PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md).

- **Tests.** `ctest` runs `url_parser`, `url_parser_diff`, `url_parser_diff_swar`
  (`URL_PARSER_NO_SIMD`), `url_parser_diff_noavx2` (x86-64 only), `url_parser_spec`,
  `url_parser_normalize`, `url_parser_next_model` and the timing guard `url_parser_perf`, which
  reports skipped unless the build is optimized and unsanitized (`-DCMAKE_BUILD_TYPE=Release`).
- **Differential and oracles.** `url_parser_diff` compares `urldecode` and both parsers with the
  frozen byte-at-a-time decoder in `test/urldecode_old.h` (never edited). Beside it,
  `url_parser_spec` checks against a decoder written from the documented semantics,
  `url_parser_normalize` against a table of Python's `os.path.normpath` output, a model and fs's
  loop, and `url_parser_next_model` against the byte loops the scans replace.
- **Sanitizers.** CI (`.github/workflows/ci.yml`, which calls the shared
  [cmake.yml](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml))
  runs every test under ASan+UBSan and under TSan, clang on Linux, and builds the benchmark with
  ada. The tests put inputs in exactly-sized heap blocks, so ASan sees any over-read:

  ```sh
  CC=clang CXX=clang++ cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
  ```

- **Mutants.** `python3 tools/mutants.py` copies the repository to a scratch directory, applies
  each mutant of `url_parser.cc`, rebuilds, runs the suite, and requires the kill and the test
  that names it: `D1` to `D14` (the decoder) by `url_parser_spec`, `S1` to `S5` (the vector
  scans of `QueryParser::next` and `PathParser::next`) by `url_parser_next_model`, `N1` to `N9`
  (`normalize_path` and `plain_path`) by `url_parser_normalize`. Each is an exact-anchor
  replacement that must match once, so a change to the code it breaks moves the anchor and
  `url_parser_mutants_anchors` fails until the script follows. One build and one ctest run per
  mutant (`--list`, `--only D1,S`, `--jobs N`, `--cmake-arg=-DCMAKE_CXX_COMPILER=clang++`); the
  `mutants` workflow (`workflow_dispatch`) runs it on Linux and macOS.
- **Fuzzing.** `cmake -B build-fuzz -DURL_PARSER_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++` (a clang
  with libFuzzer) builds `url_parser_fuzz` under ASan+UBSan, which checks `urldecode` with fuzzed
  substitutes and both parsers' buffers against the frozen decoder. No corpus is committed:
  `./build-fuzz/url_parser_fuzz -dict=fuzz/url.dict corpus/`.
- **Benchmark.** `-DURL_PARSER_BENCH=ON` builds `url_parser_bench` (`bench/bench_url.cc`): each
  row timed twice (`current #1`, `current #2 (A/A)`) beside the frozen decoder, a LUT decoder and
  ada's `percent_decode`, each checked against `urldecode` before it is timed. ada is fetched at a
  pinned commit for this target alone (`-DURL_PARSER_BENCH_ADA=OFF` skips it). Numbers: the
  README's Performance section and `CHANGELOG.md`. To compare two commits, build the bench at
  each and run both with `tools/ab_compare.py` from a clone of
  [cpp-libraries](https://github.com/Kronuz/cpp-libraries) beside this one; it runs A, B and A
  again (the A/A control) every round:

  ```sh
  git worktree add ../url-parser-base BASE_COMMIT
  cmake -S ../url-parser-base -B build-base -DCMAKE_BUILD_TYPE=Release -DURL_PARSER_BENCH=ON && cmake --build build-base --target url_parser_bench
  cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DURL_PARSER_BENCH=ON && cmake --build build-bench --target url_parser_bench
  python3 ../cpp-libraries/tools/ab_compare.py run --rounds 7 --include "current #1" \
    build-base/url_parser_bench build-bench/url_parser_bench
  ```

- **Sweep.** `bench_url.cc` times the entry points at the sizes the README shows; the sweep
  (`bench/sweep.cc`) times `urldecode` (default and query substitution set), `QueryParser::init`
  with three lookups and `PathParser::init` + `next` at 4 to 4096 bytes on clean, few-escape,
  25%-escape, separator and query-like input, against the library as the tree has it (timed twice:
  the A/A column) and the commits the CHANGELOG compares with: the original (`orig`, 4424e25, with
  `test/urldecode_old.h` for `urldecode`), the one before the small-input work (`prev`, 9ce4c23),
  and any other (`base`). The variants are fetched at configure time and built under other names
  into one binary, interleaved round by round. CI builds it and runs a few rows once.

  ```sh
  cmake -B build-sweep -DCMAKE_BUILD_TYPE=Release -DURL_PARSER_SWEEP=ON [-DURL_PARSER_SWEEP_BASE=COMMIT]
  cmake --build build-sweep --target url_parser_sweep
  build-sweep/url_parser_sweep "" 9 0 1024 0 op     # filter rounds mixed nmax parts variants (o p b)
  ```

  Time on an otherwise idle machine, under a hold, and check that the binary is the platform's own
  architecture first.

## Dependency

`url_parser.cc` has no dependencies of its own (it decodes hex through a local
table). CMake still fetches the sibling
[char-classify](https://github.com/Kronuz/char-classify) library and links it
`PUBLIC`, for consumers that use its include path and because
`test/urldecode_old.h` (the byte-at-a-time reference decoder, the oracle for
`test/urldecode_diff.cc`) uses `chars::hexdec`.

## Standalone vs. Xapiand

This is a standalone extraction from
[Xapiand](https://github.com/Kronuz/Xapiand). `url_parser.h` matches upstream
verbatim. `url_parser.cc` differs from upstream in three ways:

- **`normalize_path` inlined.** Upstream includes `"fs.hh"` for the single call
  `normalize_path(p, false, true)` in `PathParser::init`. That function is pure
  string manipulation (no filesystem access), so both overloads sit in an anonymous
  namespace at the top of this file as the fs library's `fs.cc` has them (Xapiand's
  `src/fs.cc` plus fs's fixes of the empty path, the null view and the first
  component of a relative path), and there is no `"fs.hh"` include. Keep the copyright attribution comment on the
  copied helper.
- **No trace logging.** Upstream includes `"log.h"`, defines `L_URL_PARSER`, and
  traces the parse path (including a `repr(path)` dump and two state-name-dumping
  lambdas in `init` / `next`). This copy has none of them; the `switch (cn)` that
  follows them upstream is unchanged. There is no `L_*` / `log.h` / `opts` /
  `config` / `repr` reference.
- **Single-pass decoder.** `urldecode` finds the bytes that need work with a vector
  scan (64 bytes per step on NEON and SSE2, 128 over long clean stretches with AVX2 on x86-64,
  8 with the SWAR fallback), decodes each
  block from its bit mask, and the parsers fill both buffers in one pass (see
  `ARCHITECTURE.md`). Its output matches the byte-at-a-time decoder in
  `test/urldecode_old.h`.

Keep any change reconcilable with upstream as a plain edit on top of those deltas.

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
  must give the bytes of the fs library's (`github.com/Kronuz/fs`, `fs.cc`) for every input (it is
  the same loop); it is pure string work, so don't add any filesystem access to it. `PathParser::init`
  does not call it for a path with no `.` and no `//` (`plain_path`), which is its own normal form: the
  path is decoded from where it is, with no temporary string. `test/normalize_path_test.cc` holds fs's loop
  verbatim, the original loop frozen, a model of `os.path.normpath` and a table of its output; when fs
  changes, change both copies and that test together. The contract is `os.path.normpath`'s (owner
  ruling, 2026-11-01): the current directory is `.` for `""`, `.`, `./`, `x/..`, `x/../` and `./x/..`. It
  is not RFC 3986's `remove_dot_segments`.

- **Wide loads and stores are bounded by the bytes left.** `Scanner::find` loads 64 bytes
  only with 64 left and 16 with 16 left; `decode_core` handles a block from its mask only
  with 32 bytes left (the clean run before a special is stored with a 16-byte copy that
  starts inside the block), and the output never outruns the input (`o - o1 <= p - start`), which is what lets every store land
  inside a buffer of the input's size. Change one bound and the others must move with it;
  the tests run every length from 0 to 150 and the sizes around 512 with a special at each
  position, on exactly-sized heap blocks, so ASan sees an over-read.
- **The small and mid-size paths pick their buffer by size, per platform.** Under one block the input is
  scanned (on x86-64 two overlapping 8-byte words from 8 bytes and a byte loop below; on arm64 the
  byte loop) and decoded into a
  block-sized stack buffer; one block exactly does the same after `find`; from `stack_min` (17 on
  x86-64, 48 on arm64) to 512 bytes `decode_on_stack` decodes into a 512-byte stack buffer; everything
  else decodes in place. The thresholds come from timing the i9 and the M4 and disagree, so do not
  merge them. `decode_core` is `noinline` under GCC only (inlined into `QueryParser::init` it slows GCC
  by a quarter, and it slows Apple clang when forced out), and on x86-64 its mask pass takes the first
  4 KB of a call (`mask_limit`).
- **The block loop is the clean-input path; keep it as it is.** The two-buffer decoder runs it
  until the first byte to decode and only then the mask pass: the mask pass costs about a quarter
  more than the block loop on a clean block (two loop conditions and a spilled bound per
  16 bytes, measured on the i9 and the M4), which showed as 0.8x on clean 128 B to 1 KB
  queries. Long clean stretches go through `copy_clean_run`, which stays out of line: a
  64-byte group written inline in the block loop made clean 16 to 128 byte inputs 10 to 20%
  slower on the i9 (Apple clang), and an 8-byte word step for the last 8 to 15 bytes made a
  128-byte one twice as slow on an M1 Pro, because each changed what the compiler did with the
  loop. Time a change to this loop on 8 B to 1 KB clean inputs on both chips before keeping it.
- **The AVX2 skip only skips.** `skip_clean_avx2` (the scan) and `copy_clean_avx2` (the bulk copy of
  clean runs) move over 128-byte steps whose filter finds no candidate and stop at the first step
  with one; the SSE2 code then locates the needle exactly.
  The two-needle filter is a superset test (`(byte | (a ^ b)) == (a | b)`), so it may flag a
  byte that is not a needle but never misses one; do not make it the final answer. A function
  with `target("avx2")` cannot be reached from code compiled without it except through the
  `has_avx2()` check, and nothing inside it may be a lambda (it would not get the target). The
  default build and `url_parser_diff_noavx2` both run the differential test.
- **The byte loops of the parsers are alignment-sensitive, so they start aligned.** The loops of
  `PathParser::init` and `next`, `QueryParser::next` and `normalize_path` ran 5 to 20% slower or
  faster on the i9 depending on what came before them in the object (a change to the decoder moved
  `PathParser::init+next` of a 256-byte path from 0.72 to 0.87 us, with no edit to the parser). On
  x86-64 these functions, `decode_core` and `urldecode2` are `URL_PARSER_ALIGN` (32 bytes), which makes
  the result the same in every binary; on the M4 the same lottery moved `QueryParser` by up to 1.7x,
  and the long-field skip in `next` (`find_byte3`) is what takes the loop out of it. Compare any
  edit with the start in at least two binaries (the repo bench and a second main) before believing a
  3% row.
- **The mask pass relies on the needles and the hex digits being disjoint.** A decoded
  escape consumes its two digits, and the rest of the block's mask is still right only
  because no needle is a hex digit. Adding a needle (`a`-`f`, `A`-`F`, `0`-`9`) breaks it.
- **The input classes the differential test must see.** Every subset of the four separators
  remapped, with and without `encoded` (a decoder that forgets one separator differs only on
  those sets), and an escape cut by the end at every position.

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
