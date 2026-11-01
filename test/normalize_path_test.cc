// normalize_path, the helper PathParser::init runs before it parses (normalize_path(p, false, true)), against
// the loop of the fs library it is copied from, a model of Python's os.path.normpath, and the frozen loop it
// replaced.
//
// The contract is os.path.normpath's lexical normal form (owner ruling, 2026-11-01): the current directory has
// one spelling, ".", for "", ".", "./", "x/..", "x/../" and "./x/.."; a ".." removes the component before it,
// the first of a relative path included; a relative path keeps the ".." it cannot remove ("../x" stays); the
// root absorbs a ".." ("/.." is "/"); a leading "./" is dropped; a component of three or more dots is a name;
// a run of leading '/' is one root. The trailing '/' is the flags' policy: none, `slashed` adds one,
// `keep_slash` keeps the input's. It is not RFC 3986's remove_dot_segments, which maps the relative "a/../b"
// to "/b".
//
// Before, the empty path was "" with keep_slash and "/" without, "." with keep_slash was "", "x/.." was "x" (the
// pop loop stopped one byte above the start of the output to protect the root, so it never removed the first
// component), and with keep_slash the last component was left as written ("a/.." stayed).
//
// The frozen loop is the original, never edited. For every non-empty input without a '.' the new loop must give
// its bytes; the fs loop below is the source the library's copy must equal for every input.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// Compiled together with the implementation to reach the helper in its anonymous namespace and the
// parser's buffer.
#define private public
#define class struct  // the parsers are classes with default-private buffers
#include "../url_parser.cc"
#undef class
#undef private

static int failures = 0;
#define CHECK(cond)                                                              \
	do {                                                                         \
		if (!(cond)) {                                                           \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                          \
		}                                                                        \
	} while (0)

// The pointer-range normalize_path before the fixes, frozen.
static size_t frozen_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	int levels = 0;
	char* ret = dst;
	char ch = '\0';
	const char* last = keep_slash ? end - 1 : end;
	while (src <= last) {
		ch = src == end ? '/' : *src;
		++src;
		if (ch == '.' && (levels != 0 || dst == ret || *(dst - 1) == '/' )) {
			*dst++ = ch;
			++levels;
		} else if (ch == '/') {
			while (levels != 0 && dst > ret + 1) {
				if (*--dst == '/') {
					--levels;
				}
			}
			if (dst == ret || *(dst - 1) != '/') {
				*dst++ = ch;
			}
		} else {
			*dst++ = ch;
			levels = 0;
		}
	}
	if (ch == '.' && levels == 1) {
		ch = *--dst;
	}
	if (dst > ret + 1 && !keep_slash) {
		if (slashed) {
			if (ch != '/') {
				*dst++ = '/';
			}
		} else {
			if (ch == '/') {
				--dst;
			}
		}
	}
	return dst - ret;
}

static std::string frozen_normalize(std::string_view src, bool slashed, bool keep_slash) {
	std::string dst;
	dst.resize(src.size() + 1);
	dst.resize(frozen_normalize_path(src.data(), src.data() + src.size(), &dst[0], slashed, keep_slash));
	return dst;
}

// The fs library's loop (fs.cc), which url_parser.cc's copy has to give the bytes of for every input.
static size_t fs_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	char* const ret = dst;
	const bool absolute = src < end && *src == '/';
	const bool had_slash = src < end && end[-1] == '/';
	if (absolute) {
		*dst++ = '/';
	}
	char* const base = dst;
	while (src < end) {
		const char* name = src;
		while (src < end && *src != '/') {
			++src;
		}
		const size_t size = static_cast<size_t>(src - name);
		if (src < end) {
			++src;
		}
		if (size == 0 || (size == 1 && name[0] == '.')) {
			continue;
		}
		if (size == 2 && name[0] == '.' && name[1] == '.') {
			char* last = dst;
			while (last > base && last[-1] != '/') {
				--last;
			}
			if (dst > base && !(dst - last == 2 && last[0] == '.' && last[1] == '.')) {
				dst = last > base ? last - 1 : base;
				continue;
			}
			if (absolute) {
				continue;
			}
		}
		if (dst > base) {
			*dst++ = '/';
		}
		for (size_t i = 0; i < size; ++i) {
			*dst++ = name[i];
		}
	}
	if (dst == ret) {
		*dst++ = '.';
	}
	if ((keep_slash ? had_slash : slashed) && dst[-1] != '/') {
		*dst++ = '/';
	}
	return static_cast<size_t>(dst - ret);
}

static std::string fs_normalize(std::string_view src, bool slashed, bool keep_slash) {
	std::string dst;
	dst.resize(src.size() + 2);
	dst.resize(fs_normalize_path(src.data(), src.data() + src.size(), &dst[0], slashed, keep_slash));
	return dst;
}

// Runs f in a child process: its exit status, or -1 when a signal killed it (a crash).
template <typename F>
static int in_child(F f) {
	std::fflush(nullptr);
	pid_t pid = ::fork();
	if (pid == 0) {
		::_exit(f());
	}
	int status = 0;
	if (pid < 0 || ::waitpid(pid, &status, 0) != pid) {
		return -2;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The reference: Python's os.path.normpath written out as a stack of components (the table and the digest
// below are os.path.normpath's own output), with the library's two differences: any run of leading '/' is
// one root ("//x" is "/x"; POSIX lets normpath keep two), and the trailing '/' is the flags' policy.
static std::string model_normalize(std::string_view s, bool slashed, bool keep_slash) {
	const bool absolute = !s.empty() && s[0] == '/';
	std::vector<std::string> stack;
	std::size_t at = 0;
	while (at <= s.size()) {
		const std::size_t slash = s.find('/', at);
		const std::string_view name = s.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
		at = slash == std::string_view::npos ? s.size() + 1 : slash + 1;
		if (name.empty() || name == ".") { continue; }
		if (name == "..") {
			if (!stack.empty() && stack.back() != "..") { stack.pop_back(); continue; }
			if (absolute) { continue; }
		}
		stack.emplace_back(name);
	}
	std::string core = absolute ? "/" : "";
	for (std::size_t i = 0; i < stack.size(); ++i) {
		if (i != 0) { core += '/'; }
		core += stack[i];
	}
	if (core.empty()) { core = "."; }
	if ((keep_slash ? !s.empty() && s.back() == '/' : slashed) && core.back() != '/') { core += '/'; }
	return core;
}

static std::uint64_t fnv1a(std::uint64_t h, std::string_view bytes) {
	for (unsigned char c : bytes) { h ^= c; h *= 0x100000001b3ULL; }
	return h;
}

struct NormalizeCounts {
	std::size_t compared = 0, dotted = 0, changed = 0, plain = 0;
	std::uint64_t digest = 0xcbf29ce484222325ULL;
	int reported = 0;
};

// One input in the four flag combinations: equal to the reference and to the fs loop; equal to the frozen loop
// when the input is not empty and has no '.' (its results for names and slashes are unchanged); a second
// normalization changes nothing (one canonical form); a path with nothing to fold is its own form with keep_slash.
// The digest takes the results of the first three combinations in order (slashed with keep_slash is keep_slash).
static void compare_normalize(const std::string& s, NormalizeCounts& n) {
	const bool dotted = s.find('.') != std::string::npos;
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		const std::string now = normalize_path(s, slashed, keep_slash);
		const std::string old = frozen_normalize(s, slashed, keep_slash);
		const std::string want = model_normalize(s, slashed, keep_slash);
		auto fail = [&](const char* what, const std::string& expected) {
			if (n.reported++ < 20) {
				std::fprintf(stderr, "FAIL normalize_path(\"%s\", %d, %d) = \"%s\": %s \"%s\"\n", s.c_str(), slashed, keep_slash, now.c_str(), what, expected.c_str());
			}
			++failures;
		};
		if (now != want) { fail("reference gives", want); }
		if (now != fs_normalize(s, slashed, keep_slash)) { fail("fs gives", fs_normalize(s, slashed, keep_slash)); }
		if (!s.empty() && !dotted && now != old) { fail("frozen gives", old); }
		if (dotted && now != old) { ++n.changed; }
		const std::string again = normalize_path(now, slashed, keep_slash);
		if (again != now) { fail("a second normalization gives", again); }
		if (!s.empty() && plain_path(s.data(), s.data() + s.size())) {
			if (keep_slash && now != s) { fail("a plain path is its own form, not", s); }
			if (flags == 2) { ++n.plain; }
		}
		if (flags < 3) { n.digest = fnv1a(n.digest, now + "\n"); }
		++n.compared;
	}
	if (dotted) { ++n.dotted; }
}

// normalize_path over every string of "a", "b", "." and "/" up to length 8 and 30,000 random ones up to 24, and
// 20,000 of up to 60 bytes over a wider alphabet, in all four flag combinations, equals the reference and the fs
// loop, and its results over the exhaustive strings hash to the digest of os.path.normpath's. Outside the empty
// path and the paths that have a '.', the frozen loop's bytes are unchanged; with a '.' they differ from it in the
// classes the header lists, which the frozen loop got wrong or spelled with "./" or "".
static void test_normalize_path_model() {
	const char alphabet[] = {'a', 'b', '.', '/'};
	NormalizeCounts n;
	for (int length = 1; length <= 8; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= 4; }
		for (std::size_t k = 0; k < total; ++k) {
			std::string s;
			for (std::size_t m = k, i = 0; i < static_cast<std::size_t>(length); ++i, m /= 4) { s.push_back(alphabet[m % 4]); }
			compare_normalize(s, n);
		}
	}
	// The digest of os.path.normpath's results (Python 3, posixpath, with a run of leading '/' as one root and the
	// trailing '/' policy of the flags) over the same 87,380 strings and 262,140 results.
	CHECK(n.digest == 0xef8fe9971d37494bULL);
	compare_normalize("", n);
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	for (int round = 0; round < 30000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 24; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(alphabet[(m >> 33) % 4]); }
		compare_normalize(s, n);
	}
	// Longer strings over a wider alphabet, where runs of ordinary bytes are common.
	const char wide[] = {'a', 'b', 'c', '.', '/', '/', '.', 'a', '-', '_'};
	for (int round = 0; round < 20000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 60; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(wide[(m >> 33) % sizeof wide]); }
		compare_normalize(s, n);
	}
	std::printf("normalize_path: %zu compared, %zu have a '.', %zu of those differ from the frozen loop, %zu plain\n", n.compared, n.dotted, n.changed, n.plain);
	CHECK(n.compared > 400000);
	CHECK(n.changed > 0);
	CHECK(n.plain > 0);
}

// os.path.normpath's results for named inputs (Python 3, posixpath): the current directory in every spelling is
// ".", a ".." pops the first component and the one before it, a relative path keeps the ".." it cannot pop, and
// the root absorbs it. Columns: input, default, slashed, keep_slash. Fails with the previous loop, which gave ""
// for the empty path and for "." with keep_slash, and "x" for "x/..".
static void test_normalize_path_table() {
	struct Row { const char* in; const char* plain; const char* slashed; const char* kept; };
	static const Row rows[] = {
		{"", ".", "./", "."},
		{".", ".", "./", "."},
		{"./", ".", "./", "./"},
		{"./.", ".", "./", "."},
		{"x/..", ".", "./", "."},
		{"x/../", ".", "./", "./"},
		{"./x/..", ".", "./", "."},
		{"x/y/..", "x", "x/", "x"},
		{"x/y/../..", ".", "./", "."},
		{"x/../..", "..", "../", ".."},
		{"x/../../y", "../y", "../y/", "../y"},
		{"../x", "../x", "../x/", "../x"},
		{"../../x", "../../x", "../../x/", "../../x"},
		{"../x/..", "..", "../", ".."},
		{"x", "x", "x/", "x"},
		{"x/", "x", "x/", "x/"},
		{"./x", "x", "x/", "x"},
		{"x/./y", "x/y", "x/y/", "x/y"},
		{"x//y", "x/y", "x/y/", "x/y"},
		{"x/y/", "x/y", "x/y/", "x/y/"},
		{"...", "...", ".../", "..."},
		{".../x", ".../x", ".../x/", ".../x"},
		{"a/.../b", "a/.../b", "a/.../b/", "a/.../b"},
		{".a/..", ".", "./", "."},
		{"a./..", ".", "./", "."},
		{"x/.", "x", "x/", "x"},
		{"x/./", "x", "x/", "x/"},
		{"././x", "x", "x/", "x"},
		{"..", "..", "../", ".."},
		{"../", "..", "../", "../"},
		{"../..", "../..", "../../", "../.."},
		{"x/y/../../..", "..", "../", ".."},
		{"x/y/../z/..", "x", "x/", "x"},
		{"x/y/../z", "x/z", "x/z/", "x/z"},
		{"a/b/../../c/d", "c/d", "c/d/", "c/d"},
		{"/", "/", "/", "/"},
		{"//", "/", "/", "/"},
		{"///", "/", "/", "/"},
		{"/.", "/", "/", "/"},
		{"/./", "/", "/", "/"},
		{"/..", "/", "/", "/"},
		{"/../", "/", "/", "/"},
		{"/../..", "/", "/", "/"},
		{"/x", "/x", "/x/", "/x"},
		{"/x/", "/x", "/x/", "/x/"},
		{"/x/..", "/", "/", "/"},
		{"/x/../", "/", "/", "/"},
		{"/x/../y", "/y", "/y/", "/y"},
		{"/x/y/../..", "/", "/", "/"},
		{"/x/../..", "/", "/", "/"},
		{"//x", "/x", "/x/", "/x"},
		{"/x//y/./z/../..", "/x", "/x/", "/x"},
		{"/./x", "/x", "/x/", "/x"},
		{"/..x", "/..x", "/..x/", "/..x"},
		{"/x/..y/..", "/x", "/x/", "/x"},
	};
	for (const Row& row : rows) {
		const std::string plain = normalize_path(row.in, false, false), slashed = normalize_path(row.in, true, false), kept = normalize_path(row.in, false, true);
		if (plain != row.plain || slashed != row.slashed || kept != row.kept) {
			std::fprintf(stderr, "FAIL normalize_path(\"%s\"): \"%s\" \"%s\" \"%s\", expected \"%s\" \"%s\" \"%s\"\n", row.in, plain.c_str(), slashed.c_str(), kept.c_str(), row.plain, row.slashed, row.kept);
			++failures;
		}
	}
	// One canonical form of the current directory, whatever its spelling.
	for (const char* spelling : {"", ".", "./", "././", "x/..", "x/../", "./x/..", "x/y/../..", "a/./b/../.."}) {
		CHECK(normalize_path(spelling, false, false) == ".");
		CHECK(normalize_path(spelling, true, false) == "./");
	}
}

// The current directory is "." however it is spelled, and a default-constructed std::string_view (a null data
// pointer) is the empty path, "." as well.
static void test_empty() {
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		CHECK(normalize_path("", slashed, keep_slash) == (slashed && !keep_slash ? "./" : "."));
		CHECK(normalize_path(std::string_view(), slashed, keep_slash) == normalize_path("", slashed, keep_slash));
	}
	CHECK(normalize_path("", true, false) + "vol.0" == "./vol.0");
	CHECK(normalize_path("/", false, false) == "/");
	CHECK(normalize_path(".", false, false) == ".");
	CHECK(normalize_path("./", true, false) == "./");
}

// A default-constructed std::string_view (null data pointer) is an empty path like "": the loop forms no
// pointer before the range, so it neither crashes nor reads outside the buffer. PathParser::init passes
// exactly such a view when it is given none.
static void test_null_view() {
	for (int flags = 0; flags < 4; ++flags) {
		bool slashed = flags & 1, keep_slash = flags & 2;
		int status = in_child([&] { return normalize_path(std::string_view(), slashed, keep_slash) == normalize_path("", slashed, keep_slash) ? 0 : 1; });
		if (status != 0) {
			std::fprintf(stderr, "FAIL normalize_path(std::string_view(), %d, %d): %s\n", slashed, keep_slash, status == -1 ? "crashed" : "differs from \"\"");
			++failures;
		}
	}
	int status = in_child([] {
		PathParser pp;
		pp.init(std::string_view());
		return pp.path.empty() ? 0 : 1;
	});
	if (status != 0) {
		std::fprintf(stderr, "FAIL PathParser::init(std::string_view()): %s\n", status == -1 ? "crashed" : "left a path");
		++failures;
	}
}

// What PathParser makes of the paths whose normal form changed. The parser asks for keep_slash. The current
// directory, however it is spelled, is no path to the parser: it is the empty path ("" and "." parsed to the
// empty path before, "a/.." to the path "a/.."). The unterminated last component is resolved now ("a/b/.." is
// the path "a", it stayed "a/b/.." before), and a relative path keeps the ".." it cannot pop ("../a" was "./a").
static void test_parser() {
	{
		PathParser pp;
		pp.init("a/../b");
		CHECK(pp.path == "b");
		CHECK(pp.get_id() == "b");
		pp.next();
		CHECK(pp.get_pth().empty());
	}
	{
		PathParser pp;
		pp.init("a/b/../c");
		CHECK(pp.path == "a/c");
		CHECK(pp.get_id() == "c");
		pp.next();
		CHECK(pp.get_pth() == "a");
	}
	{
		PathParser pp;
		pp.init("/a/../b");
		CHECK(pp.path == "/b");
		CHECK(pp.get_id() == "b");
	}
	// The spellings of the current directory are one path, the empty one: no id, no path.
	for (const char* spelling : {"", ".", "a/..", "./x/..", "x/y/../.."}) {
		PathParser pp;
		const PathParser::State st = pp.init(spelling);
		CHECK(pp.path.empty());
		CHECK(pp.path_decoded.empty());
		CHECK(pp.get_id().empty());
		CHECK(st == PathParser::State::PTH);
	}
	{
		PathParser pp;
		pp.init("a/../");
		CHECK(pp.path == "./");  // keep_slash keeps the input's trailing '/'
		pp.init("a/b/..");
		CHECK(pp.path == "a");
		pp.init("../a");
		CHECK(pp.path == "../a");
		CHECK(pp.get_id() == "a");
		pp.init("x/./y//z");
		CHECK(pp.path == "x/y/z");
		pp.init(std::string_view());
		CHECK(pp.path.empty());
		CHECK(pp.get_id().empty());
	}
	// A parser that is given a second path leaves nothing of the first: the buffers are resized, not cleared.
	{
		PathParser pp;
		pp.init("/idx/abcdefghij/xyz");
		pp.init("a");
		CHECK(pp.path == "a");
		CHECK(pp.path_decoded == "a");
		CHECK(pp.get_id() == "a");
		pp.init("");
		CHECK(pp.path.empty());
		CHECK(pp.path_decoded.empty());
	}
	{
		QueryParser qp;
		qp.init("alpha=1&beta=2&gamma=3");
		qp.init("a=b");
		CHECK(qp.query.size() == 3 && qp.query_decoded.size() == 3);
		CHECK(qp.next("a") == 0 && qp.get() == "b");
		qp.init("");
		CHECK(qp.query.empty() && qp.query_decoded.empty());
		CHECK(qp.next("a") == -1);
		// init rewinds: a lookup that found a key does not make the next init start after it.
		qp.init("a=1&b=2");
		CHECK(qp.next("b") == 0 && qp.get() == "2");
		qp.init("b=3&a=4");
		CHECK(qp.next("b") == 0 && qp.get() == "3");
		CHECK(qp.next("a") == 0 && qp.get() == "4");
	}
}

// The one-pass loop against the fs library's loop of 8934dbc above (a component at a time, a ".." found by
// scanning back for the last '/'), the specification, for every input in all four flag combinations: every
// string of '/', '.', 'a' and 'b' up to length 10, every string of '/', '.' and 'a' up to length 12, random
// strings of components and of bytes, and every length of a path across the size the string overload keeps on
// the stack. Source and destination are exact-size heap blocks (the contract gives `(end - src) + 2` bytes), so
// an AddressSanitizer build fails on a byte read or written past them.

// A heap block of exactly `n` bytes (at least one is allocated, so a zero-size block is a real pointer).
struct Block {
	char* data;
	explicit Block(std::size_t n) : data(static_cast<char*>(std::malloc(n ? n : 1))) {}
	~Block() { std::free(data); }
	Block(const Block&) = delete;
	Block& operator=(const Block&) = delete;
};

struct DiffCounts {
	std::size_t compared = 0;
	int reported = 0;
};

// The input in the four flag combinations through both loops; the empty input is also tried as a null range.
static void compare_one(const char* bytes, std::size_t n, DiffCounts& c) {
	Block src(n);
	if (n) { std::memcpy(src.data, bytes, n); }
	Block want(n + 2), got(n + 2);
	for (int flags = 0; flags < 4; ++flags) {
		const bool slashed = flags & 1, keep_slash = flags & 2;
		const std::size_t a = fs_normalize_path(src.data, src.data + n, want.data, slashed, keep_slash);
		const std::size_t b = normalize_path(src.data, src.data + n, got.data, slashed, keep_slash);
		++c.compared;
		if (a != b || std::memcmp(want.data, got.data, a) != 0) {
			if (c.reported++ < 20) {
				std::fprintf(stderr, "FAIL normalize_path(\"%.*s\" (%zu bytes), %d, %d) = \"%.*s\", the fs loop gives \"%.*s\"\n",
					static_cast<int>(n), bytes, n, slashed, keep_slash, static_cast<int>(b), got.data, static_cast<int>(a), want.data);
			}
			++failures;
		}
	}
	if (n == 0) {
		for (int flags = 0; flags < 4; ++flags) {
			const bool slashed = flags & 1, keep_slash = flags & 2;
			const std::size_t a = fs_normalize_path(nullptr, nullptr, want.data, slashed, keep_slash);
			const std::size_t b = normalize_path(nullptr, nullptr, got.data, slashed, keep_slash);
			++c.compared;
			if (a != b || std::memcmp(want.data, got.data, a) != 0) {
				std::fprintf(stderr, "FAIL normalize_path(nullptr, nullptr, %d, %d)\n", slashed, keep_slash);
				++failures;
			}
		}
	}
}

// Every string of `alphabet` (a list of one-byte symbols) from length 0 up to `max_length`.
static void exhaustive(const std::string& alphabet, int max_length, DiffCounts& c) {
	const std::size_t base = alphabet.size();
	char buffer[32];
	for (int length = 0; length <= max_length; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= base; }
		for (std::size_t k = 0; k < total; ++k) {
			std::size_t m = k;
			for (int i = 0; i < length; ++i, m /= base) { buffer[i] = alphabet[m % base]; }
			compare_one(buffer, static_cast<std::size_t>(length), c);
		}
	}
}

struct Random {
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	std::uint64_t next() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }
	std::size_t below(std::size_t n) { return static_cast<std::size_t>((next() >> 11) % n); }
};

// Every exhaustive string has names of at most ten letters. These are a name of any length (up to forty
// letters), joined by one to three slashes, with "." and ".." and names of dots, a leading and a trailing run
// of slashes, now and then.
static void components(Random& r, int rounds, DiffCounts& c) {
	static const char* const names[] = {
		".", "..", "...", ".a", "a.", "..a", "a..", ".git", "a", "b", "ab", "abc", "abcdefg", "abcdefgh", "abcdefghi",
		"abcdefghijklmnop", "abcdefghijklmnopq", "0123456789abcdefghijklmnopqrstuvwxyzABCD", "\xaf", "\x2e\xaf", "\xff\xff\xff\xff\xff\xff\xff\xff",
	};
	const std::size_t count = sizeof(names) / sizeof(names[0]);
	std::string s;
	for (int round = 0; round < rounds; ++round) {
		s.clear();
		if (r.below(3) == 0) { s.append(1 + r.below(3), '/'); }
		const std::size_t parts = 1 + r.below(12);
		for (std::size_t i = 0; i < parts; ++i) {
			if (i) { s.append(1 + (r.below(8) == 0 ? r.below(3) : 0), '/'); }
			if (r.below(4) == 0) {
				s.append(1 + r.below(40), static_cast<char>("abz.0"[r.below(5)]));  // a name of one repeated byte
			} else {
				s += names[r.below(count)];
			}
		}
		if (r.below(3) == 0) { s.append(1 + r.below(3), '/'); }
		compare_one(s.data(), s.size(), c);
	}
}

// Bytes on either side of '/' (0x2f) and '.' (0x2e) in the table, high bytes, NUL and newline, among them.
static void bytes(Random& r, int rounds, DiffCounts& c) {
	static const unsigned char alphabet[] = {'/', '/', '/', '.', '.', '.', 'a', 'b', 0x00, 0x0a, 0x2d, 0x30, 0x2e, 0x2f, 0xaf, 0xae, 0x80, 0xff};
	std::string s;
	for (int round = 0; round < rounds; ++round) {
		s.clear();
		const std::size_t n = r.below(100);
		for (std::size_t i = 0; i < n; ++i) { s.push_back(static_cast<char>(alphabet[r.below(sizeof(alphabet))])); }
		compare_one(s.data(), s.size(), c);
	}
}

// The string_view overload builds its result in a buffer on the stack up to a size and on the heap above it:
// every length across that size, with inputs that give the longest result (a name and a trailing "." or a
// closing '/'), against the fs loop.
static void sizes(DiffCounts& c) {
	for (std::size_t n = 0; n <= 600; ++n) {
		std::string inputs[5];
		inputs[0].assign(n, 'a');                                          // one name
		inputs[1] = std::string(n ? n - 1 : 0, 'a') + (n ? "/" : "");      // one name and its closing '/'
		for (std::size_t i = 0; i < n; ++i) { inputs[2].push_back(i % 8 == 7 ? '/' : 'a'); }   // names of seven bytes
		for (std::size_t i = 0; i < n; ++i) { inputs[3].push_back(i % 2 ? '/' : '.'); }        // "./././" and the like
		for (std::size_t i = 0; i < n; ++i) { inputs[4].push_back("a/b/c/../"[i % 9]); }
		for (const std::string& s : inputs) {
			for (int flags = 0; flags < 4; ++flags) {
				const bool slashed = flags & 1, keep_slash = flags & 2;
				Block want(s.size() + 2);
				const std::size_t a = fs_normalize_path(s.data(), s.data() + s.size(), want.data, slashed, keep_slash);
				const std::string got = normalize_path(std::string_view(s), slashed, keep_slash);
				++c.compared;
				if (got != std::string_view(want.data, a)) {
					std::fprintf(stderr, "FAIL normalize_path(string_view of %zu bytes, %d, %d) differs from the fs loop\n", s.size(), slashed, keep_slash);
					++failures;
				}
			}
		}
	}
}

static void test_fs_differential() {
	DiffCounts c;
	exhaustive("/.ab", 10, c);
	exhaustive("/.a", 12, c);
	Random r;
	components(r, 400000, c);
	bytes(r, 400000, c);
	sizes(c);
	std::printf("normalize_path: %zu inputs and flags compared with the fs loop of 8934dbc\n", c.compared);
	CHECK(c.compared > 10000000);
}

int main() {
	test_normalize_path_model();
	test_normalize_path_table();
	test_empty();
	test_null_view();
	test_parser();
	test_fs_differential();
	if (failures != 0) {
		std::fprintf(stderr, "normalize_path: %d failures\n", failures);
		return 1;
	}
	std::printf("all normalize_path tests passed\n");
	return 0;
}
