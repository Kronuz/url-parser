# Architecture

The internals of `url-parser`: how one decoded buffer feeds three small parsers,
why `PathParser` scans backward and then forward, and what `normalize_path` does
before any of that. For usage see `README.md`; for the repo map and invariants
see `AGENTS.md`.

## Shape

A header of declarations and a single compiled translation unit:

```
  url_parser.h    declares urldecode() and the QueryParser / PathParser classes
  url_parser.cc   urldecode, the two parsers, and the inlined normalize_path
```

Everything works off a decoded copy of the input. Each parser holds two strings:
one decoded with the delimiters remapped to control bytes so the scan can find
them unambiguously (`query` / `path`), and one decoded normally for handing back
values to the caller (`query_decoded` / `path_decoded`). The two strings are the
same length, so an offset found in the scan buffer indexes straight into the
value buffer.

## urldecode

`urldecode` walks the input once. A scanner looks for the bytes that need work: `%` and
`+`, plus `&`, `;`, `=` only when their substitutes differ from themselves (a set of two or
five needles, chosen per call). The scan is NEON or SSE2 (16 bytes per vector, 64 per step
in `find`) or, without either, a 64-bit SWAR test (8 bytes per step). On NEON the needles
sit in a 16- or 32-entry table and a byte is a needle when the table lookup of
`byte - lowest needle` is set, two instructions per vector; on SSE2 two needles take an or and
a compare per vector (a filter that also flags a few other bytes, then an exact test) and
five take five compares. On x86-64 with GCC or clang, a clean stretch of 256 bytes or more is
first skipped 128 bytes per step with AVX2 (`skip_clean_avx2`, used when `has_avx2()` finds
the CPU supports it); the skip only moves over steps that cannot hold a needle, and the SSE2
code then locates the first needle exactly.

Input with nothing to decode is one copy, made after the scan (no zero-fill). Otherwise
the clean prefix is copied and `decode_core` takes over. `decode_core` has two loops. The block
loop tests one 16-byte block for a needle, stores the block, and moves on when it holds none:
on clean input it is the whole of the work. The mask pass turns the test of a block into a bit
mask and handles each special byte of the block from its bit: the clean run before it is stored
16 bytes at a time (a run is shorter than a block), and a `%` followed by two hex digits is
decoded through a 256-entry nibble table and consumes them. The two digits are never needles, so
the rest of the block's mask stays valid; a `%` without two hex digits stays literal. A block
with no special byte is one load and one store. The string decoders start at a byte to decode
and run the mask pass at once; the two-buffer decoder of the parsers, whose input is often
clean, runs the block loop until it meets one and then the mask pass. A clean stretch of 256
bytes or more, at the start of the two-buffer decoder's input or after a clean block of the
mask pass, is copied by `copy_clean_run`, 64 bytes per step (128 with AVX2 on x86-64), until a
step may hold a needle. Fewer than 32 bytes left go through the block loop (the last bytes,
fewer than a block, through a byte loop). Every wide load and store is guarded by the bytes
left, and the output never outruns the input, so the over-copy fits the buffer. The separator arguments (`plus`, `amp`, `colon`, `eq`) are what
each delimiter decodes to, whether literal or produced by `%XX`, and the `encoded` byte, when
set, stands in for a decoded byte unless it is one of `{ , }`.

The result is built once. An input shorter than a block is scanned first (on x86-64 two 8-byte words
from 8 bytes up, a byte loop below and on arm64) and, when it is clean, copied in one piece; otherwise the clean
prefix and the decoded rest go through a block-sized stack buffer into a string of the exact size.
Larger inputs with something to decode are decoded into a 512-byte stack buffer up to 512 bytes
(from 17 bytes on x86-64, from 48 on arm64, below which the in-place decode is faster there) and
into a string sized to the input and shrunk at the end above that. On x86-64 the mask pass handles
the first 4 KB of a call and the block loop the rest.

The two parsers need two decodings of the same bytes: once with the delimiters
mapped to control bytes (`\1` for `=`, `\0` for `&`, and so on), so the scan can
switch on a single byte, and once normally to get the buffer whose substrings are
returned to the caller. Both strings have the same length and layout, so one pass
(`urldecode2`) fills both.

## QueryParser

`init()` decodes the query string both ways in one pass. `next(name)` then scans the
delimiter-mapped buffer for a `name=` whose key matches, tracking the start of
the current field and the position of its `=`. On a match it records the value's
offset and length and returns `0`; if it reaches the end without a match it
returns `-1`. `get()` turns the recorded offset/length into a `string_view` over
the *normally* decoded buffer, so the caller sees the real value. A key with no
`=` is a valid hit with an empty value. The scan visits each byte, except that a
field of 64 bytes or more is crossed 16 bytes at a time to the next `\0` or `\1`
(`find_byte3`), and so is a value to its `\0`: the bytes in between change nothing.

## PathParser

The path parser runs in two passes because a Xapiand path packs several optional
pieces into one string: a path, optionally `path@host` naming a node, a last-segment id,
an optional `{...}` field selector, and an optional `:command` suffix.

`init()` scans **backward** from the end. Going right-to-left is what makes the
optional trailing pieces tractable: the id is whatever sits after the last `/`,
the `:command` and the `{...}` selector hang off the tail, and a small state
machine (the `State` enum) tracks which of those it is currently inside. It
records offsets and lengths for the id, the command, and the selector, and
stops at the `/` that begins the last segment.

`next()` then scans **forward** over what is left in front of the id. By default
that span is the path (`State::PTH`); if it hits an `@`, everything before it is
the path and everything after is the host (`State::HST`). Each call yields one
segment and advances; when there is nothing left it returns `State::END`. The
forward scan clamps its end to whichever of the command / selector / id offsets
the backward pass found, so the two passes never overlap. With 64 bytes or more
left it first crosses them 16 bytes at a time to the first `\0`, `,` or `@`.

`has_pth()` answers whether there is a real path in front of the id, command, or
selector, which the caller uses to decide whether a path component is present at
all.

## normalize_path

Before either pass, `init()` runs the path through `normalize_path`. This helper
is copied from Xapiand's `src/fs.cc`, as the fs library has it. It lives among
filesystem helpers there, but it touches nothing on disk: it is a single pass over
the buffer that pops a component for each `..`, collapses repeated `/`, and trims (or,
with `keep_slash`, keeps) a trailing slash. A `..` pops the first component of a
relative path like any other (`a/..` is `.`, `a/../b` is `b`); the root and a leading
`.` or `..` are never popped, so the result cannot climb above them. An empty path,
and a default-constructed view, is empty. `url_parser` calls it as
`normalize_path(p, false, true)`, so doubled slashes and `..`/`.` segments are gone
before the parser ever looks at the string. Inlining it is the whole reason this
library has no `fs` dependency.

## Why this shape

The parser exists to turn one request line into the handful of fields a request
dispatcher needs, cheaply and without a regex or an allocation per field. The
delimiter-remapping trick lets each scan switch on a single byte instead of
comparing multibyte separators. Keeping a second, normally-decoded buffer in
lockstep means values can be returned as `string_view`s with no extra copy. The
backward-then-forward split on the path is what lets all the trailing pieces be
optional without backtracking. `normalize_path` is inlined, the parser does no
logging, and the hex table is local. The library links char-classify only so consumers
keep its include path, and the test oracle uses it.
