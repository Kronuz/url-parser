# url-parser API reference

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
    std::string_view get_hst();      // host: the part after "@" in "path@host"
    std::string_view get_id();       // last segment
    std::string_view get_slc();      // {...} field selector
    std::string_view get_cmd();      // ":command" suffix
};
```

`init()` runs a single backward pass to peel off the id, the `:command`, and
any `{...}` selector. `next()` then runs forward over what is left, yielding the
path (`State::PTH`) or, for a `path@host` segment, the host
(`State::HST`, with the part before `@` as the path), and returns `State::END` when there is nothing more.

## Caveats

- `normalize_path` drops `..` segments instead of resolving them against the
  parent, and collapses `//`: `a/../b` parses as path `a`, id `b`, and `x//y` as
  path `x`, id `y`. This is not POSIX path resolution, and it means a request
  path can never climb above its root.
- `QueryParser::get()` returns a view into the parser's decoded buffer, valid
  until the next `init()`.
