# Source layout

What each source file of url-parser holds. The rules for splitting a large file are cpp-libraries'
`docs/SPLITTING.md`.

| File | Holds |
|---|---|
| `url_parser.h` | The public API: `urldecode`, `QueryParser` and `PathParser` (120 lines). |
| `url_parser.cc` | The whole implementation, in one translation unit (1,400 lines). |
| `test/*.cc` | One test program per behavior; `test/urldecode_old.h` is the frozen reference decoder. |
| `tools/mutants.py` | The mutation run over `url_parser.cc` and its anchor check. |

## Kept whole

`url_parser.cc` (1,400 lines) stays one file. In order, it holds `normalize_path` with
`plain_path` (lines 55-165), the decoder engine (lines 168-903: the hex table, the SIMD and SWAR
scanners, the decode loops and `urldecode`), `QueryParser` (lines 906-1004) and `PathParser`
(lines 1007-1400). It stays whole for these reasons:

1. The parts are one engine. The decoder lives in an anonymous namespace, and both parsers call
   into it: `QueryParser::init` and `PathParser::init` decode through `urldecode2` and `Subst`, and
   both `next` methods scan with `find_byte3`. Moving a parser into its own translation unit would
   give that engine (about 700 lines of hot code) external linkage in a `detail` header. That is a
   linkage change on the decode loops, and it is not a move.
2. Its code layout is tuned for speed. `URL_PARSER_ALIGN` aligns the byte-loop functions to 32
   bytes because their placement in the object changes their speed on Intel cores by up to 20%. A
   single translation unit keeps that placement under one file's control.
3. The tests and the mutation tool address it as one file. Five test sources reach the private
   internals with `#include "../url_parser.cc"`. `tools/mutants.py` anchors its 28 mutants
   (decoder, scans and `normalize_path`) in `url_parser.cc`, and its anchor check is the CTest test
   `url_parser_mutants_anchors`.
4. The size doesn't call for a cut. At 1,400 lines, the three parts are each read alongside the
   decoder they share, and none of them is a unit someone would open and work on alone.

The one cut that keeps a single translation unit would move the decoder engine into a private
header that `url_parser.cc` includes, as an anonymous namespace. That header would have exactly
one includer, and `tools/mutants.py` would have to learn a second source file. The cut adds a file
without making either half easier to work on.
