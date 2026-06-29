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

`urldecode` walks the input once, writing into a buffer reserved to the input
size. A `%` triggers `chars::hexdec` from the
[char-classify](https://github.com/Kronuz/char-classify) sibling library, which
reads the two hex digits and advances the cursor. The separator arguments
(`plus`, `amp`, `colon`, `eq`) are what each escaped delimiter decodes to, and
the `encoded` byte, when set, stands in for a decoded byte unless it is one of
`{ , }`. That is the one external dependency, and the reason this library is not
header-only.

The two parsers lean on this twice. They call `urldecode` once with the
delimiters mapped to control bytes (`\1` for `=`, `\0` for `&`, and so on), so
the scan can switch on a single byte, and once normally to get the buffer whose
substrings are returned to the caller.

## QueryParser

`init()` decodes the query string both ways. `next(name)` then scans the
delimiter-mapped buffer for a `name=` whose key matches, tracking the start of
the current field and the position of its `=`. On a match it records the value's
offset and length and returns `0`; if it reaches the end without a match it
returns `-1`. `get()` turns the recorded offset/length into a `string_view` over
the *normally* decoded buffer, so the caller sees the real value. A key with no
`=` is a valid hit with an empty value.

## PathParser

The path parser runs in two passes because a Xapiand path packs several optional
pieces into one string: an optional `host@` prefix, a path, a last-segment id,
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
the backward pass found, so the two passes never overlap.

`has_pth()` answers whether there is a real path in front of the id, command, or
selector, which the caller uses to decide whether a path component is present at
all.

## normalize_path

Before either pass, `init()` runs the path through `normalize_path`. This helper
is copied from Xapiand's `src/fs.cc`. It lives among filesystem helpers there,
but it touches nothing on disk: it is a single pass over the buffer that folds
`..` segments, collapses repeated `/`, and trims (or, with `keep_slash`, keeps)
a trailing slash. `url_parser` calls it as `normalize_path(p, false, true)`, so
doubled slashes and `..`/`.` segments are gone before the parser ever looks at
the string. Inlining it is the whole reason this library has no `fs` dependency.

## Why this shape

The parser exists to turn one request line into the handful of fields a request
dispatcher needs, cheaply and without a regex or an allocation per field. The
delimiter-remapping trick lets each scan switch on a single byte instead of
comparing multibyte separators. Keeping a second, normally-decoded buffer in
lockstep means values can be returned as `string_view`s with no extra copy. The
backward-then-forward split on the path is what lets all the trailing pieces be
optional without backtracking. `normalize_path` is inlined and the trace logging
is gone, so the only thing left to link is the hex decoder in char-classify.
