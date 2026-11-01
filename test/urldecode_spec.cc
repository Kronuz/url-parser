// Specification test: urldecode, QueryParser::init and PathParser::init against a decoder
// written from the documented semantics (docs/API.md), not from the implementation or from the
// reference of test/urldecode_diff.cc.
//
// The semantics, for each input byte:
//   - `%` followed by two hex digits (both inside the input) is the byte they spell; a `%` with
//     fewer than two bytes after it, or without two hex digits, is a literal `%`.
//   - A decoded byte becomes `encoded` when `encoded` is non-zero, unless it is `{`, `,` or `}`.
//   - Then `+`, `&`, `;` and `=` (literal or produced by a `%XX`) become `plus`, `amp`, `colon`
//     and `eq`.
// The inputs: every string up to 5 bytes over an alphabet of the interesting bytes, every length
// to 200 with each special sequence at each position, lengths of 240 to 640 with a special at
// every step boundary of the bulk copy of clean runs, a long clean run between two specials,
// truncated escapes at the end of inputs of every length, random bytes, and inputs of 64 KB to
// 1 MB that end in a truncated escape. Each
// input is a heap block at a varying alignment followed by hex digits, so a read past its end
// changes the result.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>

// Compiled together with the implementation so the parsers' buffers and the
// anonymous-namespace normalize_path are reachable.
#define private public
#define class struct  // the parsers are classes with default-private buffers
#include "../url_parser.cc"
#undef class
#undef private

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)

static int hex_value(unsigned char c) {
	if (c >= '0' && c <= '9') { return c - '0'; }
	if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
	if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
	return -1;
}

static std::string spec(std::string_view in, char plus, char amp, char colon, char eq, char encoded) {
	std::string out;
	for (size_t i = 0; i < in.size(); ++i) {
		char c = in[i];
		if (c == '%' && in.size() - i >= 3) {
			const int hi = hex_value(static_cast<unsigned char>(in[i + 1]));
			const int lo = hex_value(static_cast<unsigned char>(in[i + 2]));
			if (hi >= 0 && lo >= 0) {
				c = static_cast<char>(hi * 16 + lo);
				if (encoded != '\0' && c != '{' && c != ',' && c != '}') { c = encoded; }
				i += 2;
			}
		}
		switch (c) {
			case '+': c = plus; break;
			case '&': c = amp; break;
			case ';': c = colon; break;
			case '=': c = eq; break;
			default: break;
		}
		out.push_back(c);
	}
	return out;
}

struct Params { char plus, amp, colon, eq, encoded; };

static const Params param_sets[] = {
	{' ', '&', ';', '=', '\0'},          // the defaults
	{' ', '\0', '\0', '\1', '%'},        // QueryParser::init
	{' ', '&', ';', '=', '%'},           // PathParser::init
	{'+', '&', ';', '=', '\0'},          // no remapping
	{'+', '&', ';', '=', '+'},           // `encoded` maps back to a separator
	{'_', '(', ')', '#', '='},           // `encoded` is itself a remapped byte
	{'\0', 'a', 'b', 'c', '\xff'},       // high and zero bytes
	{' ', '(', ';', '=', '\0'},          // one separator remapped
	{'+', '&', ')', '=', '\0'},
	{'+', '&', ';', '#', '%'},
};

static long checked = 0;

// `misalign` moves the block within its allocation, to vary the alignment of the input. The bytes
// after the input are hex digits: a decoder that reads past the end of a truncated escape
// ("%4" followed by one) decodes it instead of leaving it as it is.
static void check_one(const std::string& in, size_t misalign) {
	const size_t n = in.size();
	auto base = std::make_unique<char[]>(n + misalign + 2);
	char* b = base.get() + misalign;
	std::memcpy(b, in.data(), n);
	b[n] = '4';
	b[n + 1] = '1';
	for (const auto& m : param_sets) {
		const std::string want = spec(std::string_view(b, n), m.plus, m.amp, m.colon, m.eq, m.encoded);
		const std::string got = urldecode(b, n, m.plus, m.amp, m.colon, m.eq, m.encoded);
		if (got != want) {
			std::fprintf(stderr, "MISMATCH size=%zu params=%d,%d,%d,%d,%d input=", n, m.plus, m.amp, m.colon, m.eq, m.encoded);
			for (size_t i = 0; i < n && i < 64; ++i) { std::fprintf(stderr, "%02x", static_cast<unsigned char>(in[i])); }
			std::fprintf(stderr, "\n");
			std::abort();
		}
	}
	if (n <= 4096) {
		// The parsers fill both of their buffers in one pass.
		const std::string_view sv(b, n);
		QueryParser qp;
		qp.init(sv);
		CHECK(qp.query == spec(sv, ' ', '\0', '\0', '\1', '%'));
		CHECK(qp.query_decoded == spec(sv, ' ', '&', ';', '=', '\0'));
		PathParser pp;
		pp.init(sv);
		auto np = normalize_path(sv, false, true);
		if (np == ".") { np.clear(); }  // the current directory, however it is spelled, is the empty path to the parser
		CHECK(pp.path == spec(np, ' ', '&', ';', '=', '%'));
		CHECK(pp.path_decoded == spec(np, ' ', '&', ';', '=', '\0'));
	}
	++checked;
}

int main() {
	// Every string up to 5 bytes over the interesting bytes.
	const char alphabet[] = {'%', '4', '1', 'z', '+', '&', ';', '=', '\0', '\xff', 'F', 'b'};
	for (int len = 0; len <= 5; ++len) {
		size_t total = 1;
		for (int i = 0; i < len; ++i) { total *= sizeof alphabet; }
		for (size_t k = 0; k < total; ++k) {
			std::string s;
			size_t x = k;
			for (int i = 0; i < len; ++i) { s += alphabet[x % sizeof alphabet]; x /= sizeof alphabet; }
			check_one(s, k & 7);
		}
	}

	// Every length to 200, each special sequence at each position (so at every offset of a vector
	// block), and a sequence cut by the end of the input.
	const char* specials[] = {"%", "%4", "%41", "%zz", "%2b", "%2B", "%00", "+", "&", ";", "=", "%7B", "%FF", "\xff"};
	for (size_t len = 0; len <= 200; ++len) {
		for (size_t pos = 0; pos < len; ++pos) {
			for (const char* sp : specials) {
				std::string t(len, 'x');
				t.replace(pos, 1, sp);
				check_one(t, (len + pos) & 15);
				t.resize(len);  // the same, cut at `len`: an escape that runs off the end
				check_one(t, (len + pos) & 15);
			}
		}
	}

	// A clean prefix of every length, then a truncated or malformed escape.
	for (size_t len = 0; len <= 70; ++len) {
		for (const char* tail : {"%", "%4", "%41", "%4g", "%%4", "%+1"}) { check_one(std::string(len, 'a') + tail, len & 15); }
	}

	// Long inputs, where clean runs are copied 64 bytes (128 with AVX2) at a time: one special near
	// each step boundary of a clean stretch, for every length from 240 to 640.
	{
		const char* few[] = {"%41", "%", "+", "&", "\xff"};
		const size_t offs[] = {0, 1, 15, 16, 17, 31, 32, 33, 47, 48, 49, 62, 63};
		for (size_t len = 240; len <= 640; ++len) {
			for (size_t base = 0; base < len; base += 64) {
				for (size_t off : offs) {
					const size_t pos = base + off;
					if (pos >= len) { continue; }
					for (const char* sp : few) {
						std::string t(len, 'x');
						t.replace(pos, 1, sp);
						t.resize(len);
						check_one(t, (len + pos) & 15);
					}
				}
			}
			// A special in each of the last 20 bytes (the bulk run ends where fewer than 64 remain).
			for (size_t pos = len - 20; pos < len; ++pos) {
				for (const char* sp : few) {
					std::string t(len, 'x');
					t.replace(pos, 1, sp);
					t.resize(len);
					check_one(t, (len + pos) & 15);
				}
			}
		}
	}

	// A special, a long clean run, then a second special (the mask pass takes the run after the first one).
	for (size_t gap : {0, 1, 15, 16, 17, 63, 64, 65, 127, 128, 129, 200, 239, 240, 241, 255, 256, 257, 300, 511, 512, 513, 1000}) {
		for (const char* first : {"%41", "+", "&", "%4"}) {
			for (const char* second : {"%41", "+", "=", "%", "\xff"}) {
				for (size_t tail = 0; tail <= 70; tail += (tail < 20 ? 1 : 7)) {
					std::string t = std::string(first) + std::string(gap, 'x') + second + std::string(tail, 'y');
					check_one(t, (gap + tail) & 15);
					check_one(std::string(gap % 64, 'z') + t, tail & 15);  // and after a short clean prefix
				}
			}
		}
	}

	// Random bytes, and random strings weighted toward the interesting ones.
	std::mt19937_64 rng(7);
	const std::string hot = "%%++&;=0123456789abcdefABCDEF{,}";
	for (int it = 0; it < 100000; ++it) {
		const size_t len = rng() % (it % 20 == 0 ? 260 : 48);
		std::string s(len, '\0');
		const int mode = static_cast<int>(rng() % 3);
		for (auto& c : s) { c = mode == 0 ? static_cast<char>(rng()) : (rng() % 3 ? hot[rng() % hot.size()] : static_cast<char>(rng())); }
		check_one(s, rng() & 15);
	}

	// Large inputs, and inputs at the sizes where the implementation switches buffers, ending in
	// a truncated escape or a separator.
	const size_t sizes[] = {4095, 4096, 4097, 65520, 65535, 65536, 65537, 65552, 100000, (1u << 20) + 1};
	const char* tails[] = {"", "%", "%4", "%41", "%4G", "+", "&", "=", "%%", "\xff"};
	for (size_t size : sizes) {
		for (const char* tail : tails) {
			for (int kind = 0; kind < 3; ++kind) {
				std::string s;
				s.reserve(size);
				while (s.size() + std::strlen(tail) < size) {
					if (kind == 0) { s += 'a'; }
					else if (kind == 1) { s += (s.size() % 37 == 0) ? "%41" : "b"; }
					else { s += (s.size() % 5 == 0) ? "+" : (s.size() % 7 == 0 ? "%2B" : "c"); }
				}
				s.resize(size - std::strlen(tail));
				s += tail;
				check_one(s, size & 15);
			}
		}
	}

	std::printf("urldecode specification OK: %ld inputs x %zu parameter sets\n", checked, sizeof param_sets / sizeof param_sets[0]);
	return 0;
}
