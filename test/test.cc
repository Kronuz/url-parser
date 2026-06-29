// Smoke test for the standalone url-parser library.
//
// Exercises the three pieces of the public API:
//   * urldecode()   percent-decoding, "+" -> space, the encoded passthrough.
//   * QueryParser   init/next/get over a "k=v&k2=v2" query string.
//   * PathParser    the backward scan (id, command, selector) and the forward
//                   scan (path, host), plus the inlined normalize_path behavior
//                   that collapses "a/../b" and "x//y" before parsing.
//
// Build via CMake: cmake -B build && cmake --build build && ctest --test-dir build
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "url_parser.h"


// Helpers so the assertions below read as plain string comparisons against the
// string_view the parser hands back.
static std::string sv(std::string_view s) { return std::string(s); }


// ---------------------------------------------------------------------------
// urldecode: percent escapes, "+" -> space, embedded literals.
// ---------------------------------------------------------------------------

static void test_urldecode() {
	// "+" decodes to a space and %20 decodes to a space too.
	assert(urldecode(std::string_view("a%20b+c")) == "a b c");

	// A percent-escaped slash comes back as a literal "/".
	assert(urldecode(std::string_view("hello%2Fworld")) == "hello/world");

	// Plain text passes through untouched.
	assert(urldecode(std::string_view("plain-text_123")) == "plain-text_123");

	// Empty input decodes to empty.
	assert(urldecode(std::string_view("")) == "");

	std::printf("urldecode OK: %%NN, '+' -> space, passthrough, empty\n");
}


// ---------------------------------------------------------------------------
// QueryParser: find a key, read its decoded value, miss returns -1, a bare
// flag with no '=' has an empty value.
// ---------------------------------------------------------------------------

static void test_query_parser() {
	QueryParser qp;
	qp.init("foo=1&bar=hello%20world");

	// Present key with a percent-encoded value: next() returns 0 and get()
	// hands back the decoded value.
	assert(qp.next("bar", std::strlen("bar")) == 0);
	assert(sv(qp.get()) == "hello world");

	// A second key on the same query, found from a fresh scan.
	qp.init("foo=1&bar=2");
	assert(qp.next("foo", std::strlen("foo")) == 0);
	assert(sv(qp.get()) == "1");

	// A missing key returns -1.
	qp.init("foo=1&bar=2");
	assert(qp.next("missing", std::strlen("missing")) == -1);

	// A bare flag (no '=') is found, with an empty value.
	qp.init("flag&x=9");
	assert(qp.next("flag", std::strlen("flag")) == 0);
	assert(sv(qp.get()) == "");

	std::printf("QueryParser OK: hit/miss, decoded value, bare flag\n");
}


// ---------------------------------------------------------------------------
// PathParser, plain paths: the last segment is the id, the rest is the path.
// ---------------------------------------------------------------------------

static void test_path_basic() {
	// "type/id": backward scan finds id="id", forward scan finds pth="type".
	{
		PathParser pp;
		pp.init("type/id");
		assert(sv(pp.get_id()) == "id");
		assert(pp.next() == PathParser::State::PTH);
		assert(sv(pp.get_pth()) == "type");
	}

	// A deeper path keeps everything but the last segment in pth.
	{
		PathParser pp;
		pp.init("/path/to/doc");
		assert(sv(pp.get_id()) == "doc");
		pp.next();
		assert(sv(pp.get_pth()) == "/path/to");
	}

	// A trailing slash means there is no id; the whole thing is the path.
	{
		PathParser pp;
		pp.init("index/type/");
		assert(sv(pp.get_id()) == "");
		pp.next();
		assert(sv(pp.get_pth()) == "index/type/");
	}

	std::printf("PathParser basic OK: id is last segment, pth is the rest\n");
}


// ---------------------------------------------------------------------------
// PathParser, normalize_path: the inlined helper collapses ".." and "//"
// before the path is parsed. This is the fs.hh decoupling under test.
// ---------------------------------------------------------------------------

static void test_path_normalize() {
	// "a/../b" normalizes so the ".." is folded away: pth="a", id="b".
	{
		PathParser pp;
		pp.init("a/../b");
		assert(sv(pp.get_id()) == "b");
		pp.next();
		assert(sv(pp.get_pth()) == "a");
	}

	// A doubled slash "x//y" collapses to a single one: pth="x", id="y".
	{
		PathParser pp;
		pp.init("x//y");
		assert(sv(pp.get_id()) == "y");
		pp.next();
		assert(sv(pp.get_pth()) == "x");
	}

	// "a/b/../c" folds the inner "..": pth="a", id="c".
	{
		PathParser pp;
		pp.init("a/b/../c");
		assert(sv(pp.get_id()) == "c");
		pp.next();
		assert(sv(pp.get_pth()) == "a");
	}

	// "foo/./bar" drops the "." segment: pth="foo", id="bar".
	{
		PathParser pp;
		pp.init("foo/./bar");
		assert(sv(pp.get_id()) == "bar");
		pp.next();
		assert(sv(pp.get_pth()) == "foo");
	}

	std::printf("PathParser normalize OK: '..' folded, '//' collapsed, '.' dropped\n");
}


// ---------------------------------------------------------------------------
// PathParser, command and host: ":cmd" is split off as the command and the
// "host@type/id" form splits the host out in the forward scan.
// ---------------------------------------------------------------------------

static void test_path_command_and_host() {
	// "type/id:count": ":count" is the command, "id" the id, "type" the path.
	// A fresh PathParser per parse: init() does not reset a prior parse's
	// command / selector offsets, so each case gets its own instance.
	{
		PathParser pp;
		pp.init("type/id:count");
		assert(sv(pp.get_cmd()) == ":count");
		assert(sv(pp.get_id()) == "id");
		assert(pp.has_pth());
		assert(pp.next() == PathParser::State::PTH);
		assert(sv(pp.get_pth()) == "type");
	}

	// "host@type/id": the forward scan reports a host. id is still "id".
	{
		PathParser pp;
		pp.init("host@type/id");
		assert(sv(pp.get_id()) == "id");
		assert(pp.next() == PathParser::State::HST);
		assert(sv(pp.get_hst()) == "type");
		assert(sv(pp.get_pth()) == "host");
	}

	std::printf("PathParser command/host OK: ':cmd' split, '@host' split\n");
}


int main() {
	test_urldecode();
	test_query_parser();
	test_path_basic();
	test_path_normalize();
	test_path_command_and_host();
	std::printf("all url_parser tests passed\n");
	return 0;
}
