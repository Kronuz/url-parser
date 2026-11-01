// Differential test: the single-pass urldecode, QueryParser::init and
// PathParser::init against the reference decoder (urldecode_old.h) on edge cases
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

static std::vector<Params> make_param_sets() {
	std::vector<Params> v = {
		{' ', '&', ';', '=', '\0'},          // defaults
		{' ', '\0', '\0', '\1', '%'},        // QueryParser::init
		{' ', '&', ';', '=', '%'},           // PathParser::init
		{'+', '&', ';', '=', '\0'},          // identity everywhere
		{'+', '&', ';', '=', '+'},           // encoded maps back to a separator
		{'_', '(', ')', '#', '='},           // encoded is itself a mapped byte
		{'\0', 'a', 'b', 'c', '\xff'},       // high bytes
	};
	// Every subset of {plus, amp, colon, eq} remapped (each to a byte that is not a separator),
	// with and without `encoded`. A decoder that forgets one separator (its scan set or its
	// mapping) differs from the reference on exactly the sets that remap that one alone.
	for (unsigned mask = 1; mask < 16; ++mask) {
		for (char enc : {'\0', '%'}) {
			v.push_back({(mask & 1) ? ' ' : '+', (mask & 2) ? '(' : '&', (mask & 4) ? ')' : ';', (mask & 8) ? '#' : '=', enc});
		}
	}
	return v;
}
static const std::vector<Params> param_sets = make_param_sets();

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
	// The parsers fill both buffers in one pass; each must equal a separate reference decode.
	std::string_view sv(b.get(), in.size());
	QueryParser qp;
	qp.init(sv);
	CHECK(qp.query == old_urldecode(b.get(), in.size(), ' ', '\0', '\0', '\1', '%'));
	CHECK(qp.query_decoded == old_urldecode(b.get(), in.size()));
	PathParser pp;
	pp.init(sv);
	auto np = normalize_path(sv, false, true);
	if (np == ".") { np.clear(); }  // the current directory, however it is spelled, is the empty path to the parser
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

	// Every length past the widest scan step (64 bytes) and its tails, a special (or a short
	// run of them) at every position: each vector of a step, the first block after clean ones,
	// two specials in one step, and an escape cut by the end.
	for (size_t n = 0; n <= 150; ++n) {
		for (size_t pos = 0; pos < n; ++pos) {
			for (const char* sp : {"%", "+", "&", ";", "=", "%41", "%2B", "+%2B", "%+%", "=&;"}) {
				std::string s(n, 'x');
				s.replace(pos, 1, sp);
				check_one(s);
				s.resize(n);  // the same, cut at n: an escape that runs off the end
				check_one(s);
			}
		}
	}

	// Long clean stretches, which the x86 scanner skips 128 bytes per step with AVX2 (after the first
	// 16 bytes, from 272 bytes on): a special at every position, so each of the four vectors of a
	// step and each step edge is the one that holds it, and every other byte value (the filter that
	// tests two needles at once also passes a few bytes that are not needles) at the edges of steps.
	for (size_t n : {272u, 300u, 399u, 400u, 401u, 530u}) {
		const std::string clean(n, 'x');
		for (size_t pos = 0; pos < n; ++pos) {
			for (const char* sp : {"%", "+", "&", ";", "=", "%41", "%2B"}) {
				std::string t = clean;
				t.replace(pos, 1, sp);
				check_one(t);
				t.resize(n);
				check_one(t);
			}
		}
		for (int b = 0; b < 256; ++b) {
			for (size_t pos : {size_t(0), size_t(15), size_t(16), size_t(47), size_t(143), size_t(144), size_t(271), n - 1}) {
				std::string t = clean;
				t[pos] = static_cast<char>(b);
				check_one(t);
			}
		}
	}

	// Where the mask pass hands over to the block loop (4096 bytes after the first byte to decode, on
	// x86-64): a special at each position around the boundary, after a first special at a few offsets,
	// and escape-dense input that runs through it.
	for (size_t first : {size_t(0), size_t(3), size_t(16), size_t(31)}) {
		for (size_t n : {first + 4096 + 70, first + 8192 + 40}) {
			for (size_t pos = first + 4080; pos <= first + 4112; ++pos) {
				for (const char* sp : {"%", "+", "%41", "=", "%2B"}) {
					std::string t(n, 'x');
					t[first] = '+';
					t.replace(pos, 1, sp);
					check_one(t);
					t.resize(n);
					check_one(t);
				}
			}
		}
	}
	for (size_t n = 4090; n <= 4106; ++n) {
		for (const char* unit : {"%41", "a%41", "%4", "+%2B", "ab%41+"}) {
			std::string t;
			while (t.size() < n) { t += unit; }
			t.resize(n);
			check_one(t);
			t = std::string("x") + t;
			t.resize(n);
			check_one(t);
		}
	}

	// Around the size where decoding moves from the stack buffer to the string itself (512),
	// and around the next block edges: clean, one special at each of a few positions, every
	// byte special, and an escape cut by the end.
	for (size_t n : {63u, 64u, 65u, 127u, 128u, 129u, 255u, 256u, 257u, 510u, 511u, 512u, 513u, 514u, 520u, 576u, 577u, 1023u, 1024u, 1025u, 4096u, 4097u}) {
		std::string clean(n, 'x');
		check_one(clean);
		for (size_t pos : {size_t(0), size_t(1), n / 2, n - 3, n - 2, n - 1}) {
			for (const char* sp : {"%", "+", "%41", "%2B", "=", "&"}) {
				std::string t = clean;
				t.replace(pos, 1, sp);
				check_one(t);
				t.resize(n);
				check_one(t);
			}
		}
		check_one(std::string(n, '%'));
		check_one(std::string(n, '+'));
		std::string esc;
		while (esc.size() < n) { esc += "%41"; }
		esc.resize(n);
		check_one(esc);
		std::string mixed;
		while (mixed.size() < n) { mixed += "a+%2B&=;"; }
		mixed.resize(n);
		check_one(mixed);
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
	std::printf("urldecode differential OK: %zu random + edge cases x %zu param sets\n", cases, param_sets.size());
	return 0;
}
