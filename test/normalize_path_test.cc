// normalize_path, the helper PathParser::init runs before it parses (normalize_path(p, false, true)),
// against the frozen loop it replaced and against std::filesystem::path::lexically_normal.
//
// The copy follows the fs library's fixes of its normalize_path:
//   - an empty path, and a default-constructed std::string_view (a null data pointer), is the empty
//     path: the loop formed end - 1, which for a null pointer wrapped around and read from address 1;
//   - a ".." pops the first component of a relative path like any other ("a/.." is ".", "a/../b" is
//     "b"; the loop stopped one byte above the start of the output to protect the root, so "a/.." was
//     "a" and "a/../b" was "a/b"). The root is never popped, and a leading "." or ".." stays the "." the
//     output starts with, so a path cannot climb above its root or its start.
//
// The contract is the std::filesystem one, not RFC 3986's remove_dot_segments: that algorithm is
// defined for absolute paths and maps the relative "a/../b" to "/b", where the library keeps it relative.
//
// The frozen loop is the previous copy, never edited. Outside the class of paths whose result changed the
// new loop must give its bytes; inside the class, the oracle's, where the oracle and the library agree.
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

// The fs library's loop (fs.cc), byte by byte, which url_parser.cc's has to give the bytes of for every
// input: the library's loop copies a run of ordinary bytes in one inner loop.
static size_t fs_normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash) {
	int levels = 0;
	char* ret = dst;
	char ch = '\0';
	const char* begin = src;
	// The first component of the output so far is dots only (a leading "." or ".."), and is still open.
	bool first_dots = true;
	bool first_open = true;
	// A ".." popped every component, so the path is "." unless something follows.
	bool emptied = false;
	// Visit every byte of [src, end), then the closing '/' of the last component unless keep_slash.
	// An empty path has no last component, so it gets no closing '/' (that would name the root).
	// There is no end - 1, so a default-constructed view (a null src) is the empty path.
	bool closing = !keep_slash && src < end;
	while (src < end || closing) {
		bool from_input = true;
		if (src < end) {
			ch = *src++;
		} else {
			ch = '/';
			closing = false;
			from_input = false;
		}
		if (ch == '.' && (levels != 0 || dst == ret || *(dst - 1) == '/' )) {
			*dst++ = ch;
			++levels;
		} else if (ch == '/') {
			// A ".." pops back to the '/' before the component it removes. The root is never popped,
			// nor the dots of a leading "." or ".."; the first component of a relative path is popped
			// like any other, and nothing is left (the path is then "." or what follows).
			const bool open = dst != ret;
			char* const floor = open && (*ret == '/' || first_dots) ? ret + 1 : ret;
			while (levels != 0 && dst > floor) {
				if (*--dst == '/') {
					--levels;
				}
			}
			if (open && dst == ret) {
				levels = 0;
				emptied = true;
				first_dots = true;
				first_open = true;
			} else if (dst == ret ? (from_input && src == begin + 1) : (*(dst - 1) != '/')) {
				*dst++ = ch;
				if (dst - ret > 1) {
					first_open = false;
				}
			}
		} else {
			*dst++ = ch;
			levels = 0;
			if (first_open && ch != '.') {
				first_dots = false;
			}
		}
	}
	if (ch == '.' && levels == 1) {
		ch = *--dst;
	}
	if (dst == ret && emptied) {
		*dst++ = '.';
		if (slashed || (keep_slash && end[-1] == '/')) {
			*dst++ = '/';
		}
	} else if (dst > ret + 1 && !keep_slash) {
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

static std::string fs_normalize(std::string_view src, bool slashed, bool keep_slash) {
	std::string dst;
	dst.resize(src.size() + 1);
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

// The class of paths whose result changed: a relative path where a ".." (or a longer run of dots, which pops
// one component fewer than it has dots) pops every component the output holds. The frozen loop never popped
// the first component, so "a/.." was "a" and "a/../b" was "a/b"; the output is now empty at that point
// ("." if nothing follows). Components are split at '/'; each one followed by '/' is resolved, and so is the
// last one unless keep_slash leaves it as it is. A leading run of dots stays as the "." the output starts
// with and is never popped.
static bool is_dots(std::string_view c) { return !c.empty() && c.find_first_not_of('.') == std::string_view::npos; }

static std::vector<std::string_view> split_components(std::string_view s) {
	std::vector<std::string_view> out;
	std::size_t at = 0;
	for (;;) {
		std::size_t slash = s.find('/', at);
		if (slash == std::string_view::npos) { out.push_back(s.substr(at)); return out; }
		out.push_back(s.substr(at, slash - at));
		at = slash + 1;
	}
}

static bool pops_first_component(std::string_view s, bool keep_slash) {
	if (s.empty() || s[0] == '/') { return false; }
	const auto comps = split_components(s);
	const std::size_t resolved = comps.size() - (keep_slash ? 1 : 0);
	std::size_t depth = 0;
	bool base = false, out_empty = true, hit = false;
	for (std::size_t i = 0; i < resolved; ++i) {
		const std::string_view c = comps[i];
		if (c.empty()) { continue; }
		if (!is_dots(c)) { ++depth; out_empty = false; continue; }
		if (out_empty) { base = true; out_empty = false; continue; }
		const std::size_t pops = c.size() - 1;
		if (pops == 0 || depth == 0) { continue; }
		if (pops >= depth && !base) { hit = true; depth = 0; out_empty = true; }
		else { depth = pops >= depth ? 0 : depth - pops; }
	}
	return hit;
}

// The oracle for the changed class where std::filesystem::path::lexically_normal and the library agree:
// components that are names or exactly "..", a result with no leading "..", and with keep_slash a last
// component that is not an unterminated "..". lexically_normal gives "." for what is left of "a/..", and
// the library's own trailing-slash policy is applied to its answer.
static bool oracle_applies(std::string_view s, bool keep_slash) {
	const auto comps = split_components(s);
	for (auto c : comps) {
		if (is_dots(c) && c != "..") { return false; }
	}
	if (keep_slash && is_dots(comps.back())) { return false; }
	const std::string ln = std::filesystem::path(std::string(s)).lexically_normal().generic_string();
	return ln != ".." && ln.compare(0, 3, "../") != 0;
}

static std::string oracle_normalize(std::string_view s, bool slashed, bool keep_slash) {
	std::string ln = std::filesystem::path(std::string(s)).lexically_normal().generic_string();
	while (ln.size() > 1 && ln.back() == '/') { ln.pop_back(); }
	return (keep_slash ? s.back() == '/' : slashed) ? ln + "/" : ln;
}

struct NormalizeCounts {
	std::size_t compared = 0, changed = 0, applicable = 0, old_wrong = 0;
	int reported = 0;
};

// One input, one flag combination: equal to the frozen loop outside the changed class; inside it, equal to
// the oracle where the oracle applies (and the frozen loop was not); a relative path stays relative.
static void compare_normalize(const std::string& s, bool slashed, bool keep_slash, NormalizeCounts& n) {
	const std::string now = normalize_path(s, slashed, keep_slash);
	const std::string old = frozen_normalize(s, slashed, keep_slash);
	if (now != fs_normalize(s, slashed, keep_slash)) {
		if (n.reported++ < 20) { std::fprintf(stderr, "FAIL normalize_path(\"%s\", %d, %d) = \"%s\": fs gives \"%s\"\n", s.c_str(), slashed, keep_slash, now.c_str(), fs_normalize(s, slashed, keep_slash).c_str()); }
		++failures;
	}
	const bool changed = pops_first_component(s, keep_slash);
	auto fail = [&](const char* what, const std::string& want) {
		if (n.reported++ < 20) {
			std::fprintf(stderr, "FAIL normalize_path(\"%s\", %d, %d) = \"%s\": %s \"%s\"\n", s.c_str(), slashed, keep_slash, now.c_str(), what, want.c_str());
		}
		++failures;
	};
	if (!changed) {
		if (now != old) { fail("frozen gives", old); }
	} else {
		++n.changed;
		if (oracle_applies(s, keep_slash)) {
			const std::string want = oracle_normalize(s, slashed, keep_slash);
			++n.applicable;
			if (now != want) { fail("oracle gives", want); }
			if (old != want) { ++n.old_wrong; }
		}
	}
	if (s[0] != '/' && now.substr(0, 1) == "/") { fail("made a relative path absolute, from", s); }
	++n.compared;
}

// normalize_path over every string of "a", "." and "/" up to length 8 and 30,000 random ones up to 24, in
// all four flag combinations, equals the frozen implementation except in two classes: the empty path
// without keep_slash (the frozen loop appended its closing '/' to nothing and returned the root) and the
// paths where a ".." pops the first component (above). The result never starts with '/' unless the input
// does: relative stays relative.
static void test_frozen_and_oracle() {
	const char alphabet[] = {'a', '.', '/'};
	NormalizeCounts n;
	for (int length = 1; length <= 8; ++length) {
		std::size_t total = 1;
		for (int i = 0; i < length; ++i) { total *= 3; }
		for (std::size_t k = 0; k < total; ++k) {
			std::string s;
			for (std::size_t m = k, i = 0; i < static_cast<std::size_t>(length); ++i, m /= 3) { s.push_back(alphabet[m % 3]); }
			for (int flags = 0; flags < 4; ++flags) { compare_normalize(s, flags & 1, flags & 2, n); }
		}
	}
	std::uint64_t x = 0x9e3779b97f4a7c15ULL;
	for (int round = 0; round < 30000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 24; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(alphabet[(m >> 33) % 3]); }
		for (int flags = 0; flags < 4; ++flags) { compare_normalize(s, flags & 1, flags & 2, n); }
	}
	// Longer strings over a wider alphabet, where runs of ordinary bytes are common.
	const char wide[] = {'a', 'b', 'c', '.', '/', '/', '.', 'a', '-', '_'};
	for (int round = 0; round < 20000; ++round) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		std::string s;
		for (std::uint64_t m = x, i = 0; i < 1 + x % 60; ++i, m = m * 6364136223846793005ULL + 1442695040888963407ULL) { s.push_back(wide[(m >> 33) % sizeof wide]); }
		for (int flags = 0; flags < 4; ++flags) { compare_normalize(s, flags & 1, flags & 2, n); }
	}
	std::printf("normalize_path: %zu compared, %zu in the changed class, %zu with an oracle, the frozen loop wrong on %zu\n", n.compared, n.changed, n.applicable, n.old_wrong);
	CHECK(n.compared > 100000);
	// The class is not empty, the oracle reaches into it, and the frozen loop was wrong on some of what it reaches.
	CHECK(n.changed > 0);
	CHECK(n.applicable > 0);
	CHECK(n.old_wrong > 0);
}

// The first component of a relative path is popped by a ".." like any other: "a/.." is ".", "a/../b" is "b".
// The calls PathParser::init makes are the third argument true.
static void test_first_component() {
	CHECK(normalize_path("a/..", false, false) == ".");
	CHECK(normalize_path("a/..", true, false) == "./");
	CHECK(normalize_path("a/../", false, false) == ".");
	CHECK(normalize_path("a/../", false, true) == "./");
	CHECK(normalize_path("a/../b", false, false) == "b");
	CHECK(normalize_path("a/../b/", false, false) == "b");
	CHECK(normalize_path("a/../b/", true, false) == "b/");
	CHECK(normalize_path("a/../b/", false, true) == "b/");
	CHECK(normalize_path("a/b/../..", false, false) == ".");
	CHECK(normalize_path("a/b/../../c", false, false) == "c");
	CHECK(normalize_path("a/../b/..", false, false) == ".");
	CHECK(normalize_path(".a/..", false, false) == ".");
	CHECK(normalize_path("a./..", false, false) == ".");
	CHECK(normalize_path("abc/../x/y", false, false) == "x/y");
	// What is left alone: a deeper component, the root, a leading "." or "..", and a keep_slash tail.
	CHECK(normalize_path("a/b/..", false, false) == "a");
	CHECK(normalize_path("/a/..", false, false) == "/");
	CHECK(normalize_path("/a/../b", false, false) == "/b");
	CHECK(normalize_path("../a/..", false, false) == ".");
	CHECK(normalize_path("../a", false, false) == "./a");
	CHECK(normalize_path("./a/..", false, false) == ".");
	CHECK(normalize_path("a/..", false, true) == "a/..");
}

// The empty path names nothing, so it normalizes to nothing in all four flag combinations. Oracle:
// std::filesystem::path::lexically_normal maps "" to "". The frozen loop returned "/" for the two
// combinations without keep_slash (the root, where a name appended to it would land); PathParser::init
// passes keep_slash, where the result was already "".
static void test_empty() {
	const std::string oracle = std::filesystem::path("").lexically_normal().string();
	CHECK(oracle.empty());
	for (int flags = 0; flags < 4; ++flags) {
		bool slashed = flags & 1, keep_slash = flags & 2;
		CHECK(normalize_path("", slashed, keep_slash) == oracle);
		if (!keep_slash) {
			CHECK(frozen_normalize("", slashed, keep_slash) == "/");
			CHECK(frozen_normalize("", slashed, keep_slash) != normalize_path("", slashed, keep_slash));
		}
	}
	// A path that is only separators or dots is not empty: it keeps its meaning.
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

// What PathParser makes of the paths the fix changes: the parts of "a/../b" and "a/.." after the fold.
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
	{
		// The parser keeps the slash: an unterminated ".." is left as it is (the last component may be a name),
		// and a terminated one pops what is before it.
		PathParser pp;
		pp.init("a/..");
		CHECK(pp.path == "a/..");
		pp.init("a/../");
		CHECK(pp.path == "./");
	}
	{
		PathParser pp;
		pp.init("");
		CHECK(pp.path.empty());
		CHECK(pp.get_id().empty());
	}
}

int main() {
	test_frozen_and_oracle();
	test_first_component();
	test_empty();
	test_null_view();
	test_parser();
	if (failures != 0) {
		std::fprintf(stderr, "normalize_path: %d failures\n", failures);
		return 1;
	}
	std::printf("all normalize_path tests passed\n");
	return 0;
}
