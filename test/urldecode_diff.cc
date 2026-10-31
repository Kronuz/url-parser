// Differential test: the single-pass urldecode, QueryParser::init and
// PathParser::init against the frozen original (urldecode_old.h) on edge cases
// and random inputs, for the default and the remapped separator sets.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

// Compiled together with the implementation so the private buffers and the
// anonymous-namespace normalize_path are reachable.
#define private public
#define class struct  // the parsers are classes with default-private buffers
#include "../url_parser.cc"
#undef class
#undef private
#include "urldecode_old.h"

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)

struct Params { char plus, amp, colon, eq, encoded; };

static const Params param_sets[] = {
	{' ', '&', ';', '=', '\0'},          // defaults
	{' ', '\0', '\0', '\1', '%'},        // QueryParser::init
	{' ', '&', ';', '=', '%'},           // PathParser::init
	{'+', '&', ';', '=', '\0'},          // identity everywhere
	{'+', '&', ';', '=', '+'},           // encoded maps back to a separator
	{'_', '(', ')', '#', '='},           // encoded is itself a mapped byte
	{'\0', 'a', 'b', 'c', '\xff'},       // high bytes
};

static void check_one(const std::string& in) {
	// Exactly-sized heap block: a read past the end is visible to sanitizers.
	auto b = std::make_unique<char[]>(in.size() ? in.size() : 1);
	std::memcpy(b.get(), in.data(), in.size());
	for (const auto& m : param_sets) {
		auto want = old_urldecode(b.get(), in.size(), m.plus, m.amp, m.colon, m.eq, m.encoded);
		auto got = urldecode(b.get(), in.size(), m.plus, m.amp, m.colon, m.eq, m.encoded);
		if (want != got) {
			std::fprintf(stderr, "MISMATCH input=[%s] params=%d,%d,%d,%d,%d\n", in.c_str(), m.plus, m.amp, m.colon, m.eq, m.encoded);
			std::abort();
		}
	}
	// The parsers fill both buffers in one pass; each must equal the two old passes.
	std::string_view sv(b.get(), in.size());
	QueryParser qp;
	qp.init(sv);
	CHECK(qp.query == old_urldecode(b.get(), in.size(), ' ', '\0', '\0', '\1', '%'));
	CHECK(qp.query_decoded == old_urldecode(b.get(), in.size()));
	PathParser pp;
	pp.init(sv);
	auto np = normalize_path(sv, false, true);
	CHECK(pp.path == old_urldecode(np.data(), np.size(), ' ', '&', ';', '=', '%'));
	CHECK(pp.path_decoded == old_urldecode(np.data(), np.size()));
}

int main() {
	std::vector<std::string> edge = {
		"", "a", "%", "%4", "%41", "%zz", "%4z", "z%4", "+", "++", "%2B", "%2b", "%26", "%3D", "%3B", "%7B%2C%7D",
		"%00", "%25", "%%", "%%41", "%25%32%35", "a&b=c;d", "a+b%20c", "%FF%fe", "%G1", "%1G", "%-1", "%+1",
		"key=%7Bvalue%7D&other=a+b", "ok-no-specials-at-all-here-0123456789", "0123456789abcdef%", "0123456789abcdef0%4",
		std::string("a\0b%00c", 7), "\xff\xfe%ff", "&&&===;;;",
	};
	for (auto& e : edge) { check_one(e); }

	// Every length around the 8/16-byte scan steps, a special at every position.
	for (size_t n = 0; n <= 70; ++n) {
		for (size_t pos = 0; pos < n; ++pos) {
			for (const char* sp : {"%", "+", "&", ";", "=", "%41", "%2B"}) {
				std::string s(n, 'x');
				s.replace(pos, 1, sp);
				check_one(s);
			}
		}
	}

	// Random: alphabets weighted toward the interesting bytes.
	std::mt19937 rng(12345);
	const std::string alpha = "%%%++&;=0123456789abcdefABCDEFxyz{,}\0\xff";
	size_t cases = 0;
	for (int iter = 0; iter < 60000; ++iter) {
		size_t n = rng() % (iter % 50 == 0 ? 300 : 40);
		std::string s;
		int density = 1 + rng() % 4;
		for (size_t i = 0; i < n; ++i) {
			unsigned r = rng();
			if (r % 8 < (unsigned)density) { s += alpha[(r >> 8) % alpha.size()]; }
			else { s += char('a' + (r >> 8) % 26); }
		}
		check_one(s);
		++cases;
	}
	// Fully random bytes.
	for (int iter = 0; iter < 20000; ++iter) {
		std::string s(rng() % 64, '\0');
		for (auto& c : s) { c = static_cast<char>(rng()); }
		check_one(s);
		++cases;
	}
	std::printf("urldecode differential OK: %zu random + edge cases x %zu param sets\n", cases, sizeof(param_sets) / sizeof(*param_sets));
	return 0;
}
