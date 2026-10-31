# Changelog

Notable changes to url-parser, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); entries are dated because the
library has no releases.

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
