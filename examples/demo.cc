// A runnable tour of the standalone url-parser library (not a test).
//
// Build a top-level CMake project and run it:
//   cmake -B build && cmake --build build && ./build/url_parser_demo
//
// It walks the three pieces of the API: urldecode, QueryParser over a query
// string, and PathParser over a path (showing the id / path / command / host
// split and the normalize_path collapsing of ".." and "//").
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "url_parser.h"


static const char* sv(std::string_view s, std::string& buf) {
	buf.assign(s.data(), s.size());
	return buf.c_str();
}


int main() {
	std::string buf;

	// --- urldecode ---
	std::printf("urldecode:\n");
	std::printf("  'a%%20b+c'       -> '%s'\n", urldecode(std::string_view("a%20b+c")).c_str());
	std::printf("  'hello%%2Fworld' -> '%s'\n", urldecode(std::string_view("hello%2Fworld")).c_str());

	// --- QueryParser ---
	std::printf("\nQueryParser over 'foo=1&bar=hello%%20world&flag':\n");
	QueryParser qp;
	qp.init("foo=1&bar=hello%20world&flag");
	for (const char* key : {"foo", "bar", "flag", "missing"}) {
		qp.init("foo=1&bar=hello%20world&flag");
		int r = qp.next(key, std::strlen(key));
		if (r == 0) {
			std::printf("  %-8s = '%s'\n", key, sv(qp.get(), buf));
		} else {
			std::printf("  %-8s = <not present>\n", key);
		}
	}

	// --- PathParser ---
	std::printf("\nPathParser:\n");
	for (const char* path : {"type/id", "/path/to/doc", "a/../b", "x//y",
	                         "type/id:count", "host@type/id"}) {
		PathParser pp;
		pp.init(path);
		std::string id(pp.get_id());
		std::string cmd(pp.get_cmd());
		auto state = pp.next();
		std::string pth(pp.get_pth());
		std::string hst(pp.get_hst());
		std::printf("  %-16s pth='%s' id='%s' cmd='%s' hst='%s'\n",
			path, pth.c_str(), id.c_str(), cmd.c_str(), hst.c_str());
		(void)state;
	}

	return 0;
}
