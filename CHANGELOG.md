# Changelog

Notable changes to url-parser, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); entries are dated because the
library has no releases.

## 2026-10-31

### Fixed

- Short input is no slower than the byte-at-a-time decoder (UP1). On x86-64 an input of 4
  to 8 bytes with an escape took 0.75x the speed of the decoder before the rewrite (Intel
  i9, Apple clang): the scanner's set-up and the string's two resize calls cost more than
  decoding 8 bytes. An input shorter than a block now takes a byte loop that copies the
  clean prefix while it scans (on x86-64 two 8-byte words from 8 bytes up). A clean input is
  one `std::string(q, size)`; a dirty one is decoded in a block-sized stack buffer. On the
  i9, against the byte loop: 4 bytes with an escape 0.76 to 0.97-1.13x, 8 bytes with an escape
  1.0-1.2x to 1.2-1.6x, clean 4 and 8 bytes 1.3-2.5x to 1.4-2.9x; Apple M4 Pro: 4 bytes with an
  escape 0.97-1.06x to 1.28-1.38x. Test: `url_parser_perf` (short-input ratio at most 1.15 of the reference).
- The differential test sees a decoder that forgets one separator (UP2). Every subset of
  `plus`, `amp`, `colon` and `eq` is remapped, with and without `encoded`; the old parameter
  sets remapped all of them or none. A decoder without `amp`, `colon` or `eq` in its scan set
  passed the old test and fails the new one. The test also sweeps the sizes around the
  stack-buffer limits, the long clean stretches of the AVX2 skip and the 4 KB hand-over of the
  mask pass, on exactly-sized heap blocks for sanitizers; the fuzz target compares both
  parsers' buffers with the reference with fuzzed substitutes.
- The portable SWAR scanner finds the hit with the exact zero-byte test and `ctz`, not by
  rescanning the 8 bytes (the classic test has false positives above a true one, but its
  lowest set bit is exact). `url_parser_diff_swar` runs the whole test with it.
- The CMake pin comment and AGENTS.md said the library had no dependency on char-classify
  and described the scan as 16 bytes per step with SWAR; both are corrected (UP3).

### Changed

- `urldecode` is as fast as `ada::unicode::percent_decode` on clean input on arm64 and within
  3-11% of it on x86-64 (UP4). `find` takes 4 vectors per step (a table lookup per vector on
  NEON, an or and a compare on SSE2, 8 vectors per step with AVX2 chosen at run time on
  x86-64) and a clean input is one copy. The scan guard in `url_parser_perf` fails if the
  wide step falls below 1.3x the one-vector loop (3.3-5.6x on x86 with AVX2, 1.5-2.8x on arm).
- The block loop of the decoder is driven by a bit mask: one vector test per 16-byte block,
  each special byte decoded from its bit. On x86-64 it takes the first 4 KB of a call and the
  one-vector-per-step loop the rest, which keeps 64 KB of escape-dense input level with the
  old loop on Intel cores. The stack buffer is used from 17 bytes on x86-64 and from 48 on
  arm64. `decode_core` is out of line under GCC only.
- 256 bytes with 25% escapes: 1.4x (i9) and 2.4x (M4 Pro) faster than the previous decoder
  and 1.9x and 1.5x faster than the single-pass LUT decoder, which beat it on x86 before.

### Measured

`bench/bench_url.cc` with ada, `-O3`, `quiet strict`, median of two rounds, against the
previous decoder (`9ce4c23`) on the same machine; no row is more than 3% slower on either
machine:

| Row | M4 Pro before | after | i9 before | after |
|---|---:|---:|---:|---:|
| 8 bytes clean | 6.2 ns | 5.5 ns | 13.4 ns | 8.1 ns |
| 16 bytes, a few escapes | 9.8 ns | 7.8 ns | 25.6 ns | 15.9 ns |
| 256 bytes, 25% escapes | 171 ns | 72 ns | 241 ns | 175 ns |
| 4 KB clean | 170 ns | 108 ns | 252 ns | 139 ns |
| 4 KB, a few escapes and `+` | 984 ns | 466 ns | 927 ns | 920 ns |
| 4 KB, 25% escapes | 2.51 µs | 1.08 µs | 2.20 µs | 2.07 µs |
| 64 KB clean | 2.40 µs | 1.52 µs | 3.85 µs | 2.21 µs |
| 64 KB, 25% escapes | 38.1 µs | 16.9 µs | 42.8 µs | 42.6 µs |
| `QueryParser::init` + 3 lookups, 4 KB | 6.20 µs | 3.03 µs | 7.50 µs | 6.04 µs |

`ada::unicode::percent_decode` on 4 KB of clean input: 151 ns (M4 Pro, 1.4x slower than
`urldecode`) and 135 ns (i9, 3% faster); on 64 KB: 1.82 µs and 2.00 µs (1.2x slower and 10%
faster). GitHub runners, `urldecode` against ada on clean 4 KB to 64 KB: Neoverse N2 0.85-0.9x (glibc's
`memchr` is faster there), AMD EPYC 0.86-1.0x, Intel macOS 0.8-1.0x. The 8 to 32 byte clean rows
are 1.1 to 3x slower than ada's `find` plus copy everywhere.

Known: `PathParser::init` plus `next` is 8-20% slower than before on the i9 for 8 to 256 byte
paths (also on the Intel macOS runner at 16 bytes; aligning `decode_core` does not change it),
and `QueryParser::init` on 128 B to 1 KB of input with no separator or escape is 10-12% slower
on the M4 Pro. Neither is a row of `bench/bench_url.cc`; real queries contain `=` and `&`.

### Not adopted

char-classify now has `chars_simd.hh` (block scanners) and a byte hex table. `chars::hex_value`
is the same 256-byte table as the local one and compiles to the same lookup, plus one load of
its address through the GOT on macOS, so it is not faster. `FirstOf<N>::find` tests one vector
per step where `Scanner::find` tests four (8 with AVX2): 2.2x to 4.7x on the scan guard.
Neither replaces the local code.

### Added

- `test/perf_guard.cc` (CTest `url_parser_perf`): the short-input ratio and the scan ratio, each
  timed in the same process; skipped (status 77) in unoptimized and sanitized builds.
- `url_parser_diff_noavx2` (x86-64): the differential test without the AVX2 skip.
- `bench/bench_url.cc` rows at 8 and 16 bytes.

## 2026-10-30

### Changed

- `urldecode` decodes in one SIMD-scanned pass: it scans 16 bytes at a time (NEON,
  SSE2, or a 64-bit SWAR fallback) for the bytes that need work, stores each block as
  it is scanned, advances the output only past the clean bytes, and decodes `%XX`
  through a 256-entry nibble table. Input with nothing to decode is one bulk copy.
  Before, every byte went through two nested switches and `std::string::push_back`.
  (`1f6aded`)
- `QueryParser::init` and `PathParser::init` fill the delimiter-mapped and the plain
  buffer in one pass. Before, they decoded the same bytes twice. (`1f6aded`)
- Output bytes, signatures, and the separator and `encoded` semantics are unchanged.
  Apple M-series, clang 17 `-O3`, 4 KB inputs, `bench/bench_url.cc`:

  | Input | Before | After |
  |---|---:|---:|
  | Clean | 7.9 µs | 157 ns |
  | A few escapes and `+` | 7.5 µs | 0.95 µs |
  | 25% `%XX` | 6.0 µs | 2.5 µs |
  | `QueryParser::init` plus three lookups | 15.4 µs | 6.1 µs |

### Added

- `test/urldecode_diff.cc` (CTest `url_parser_diff`, plus a SWAR-only build) compares
  `urldecode` and both parsers' `init` with the byte-at-a-time reference decoder in
  `test/urldecode_old.h`: edge cases, every length around the block size with a
  special byte at each position, and random input, for seven separator sets.
- `bench/bench_url.cc` (`-DURL_PARSER_BENCH=ON`) compares with the reference, a LUT
  decoder, and `ada::unicode::percent_decode`.

## 2026-10-28

### Fixed

- `urldecode` read one or two bytes past the buffer on input ending in `%` or `%X`
  (a `q=` URL cut inside an escape, seen near 64 KiB): it handed the cursor to
  `chars::hexdec`, which reads two bytes without knowing the end. It decodes an
  escape only when two bytes remain; otherwise the `%` is literal, as for any
  malformed escape. (`07847a1`)
- `PathParser::init` called `back()` on an empty path. (`07847a1`)

### Added

- Tests place the input in an exactly-sized heap block at 65535, 65536, and 65537
  bytes with every cut form of an escape.
- A libFuzzer target (`-DURL_PARSER_FUZZ=ON`) over `urldecode`, `QueryParser`, and
  `PathParser`.
