// QueryParser::next and PathParser::next against the byte loops they replace.
//
// Both functions skip the bytes between delimiters with a vector scan when 32 or more bytes remain.
// The reference functions below are the loops as they were before the scan: they visit every byte.
// The test runs both on the same state (the fields of the parser before the call), so the result,
// the offsets and the lengths must be equal after every call, on buffers with long fields, delimiters
// at every offset of a 64-byte step, empty fields and embedded zero bytes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// Compiled together with the implementation so the parsers' fields are reachable.
#define private public
#define class struct  // the parsers are classes with default-private buffers
#include "../url_parser.cc"
#undef class
#undef private

#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); std::abort(); } } while (0)

// QueryParser::next, byte by byte.
static int
ref_query_next(QueryParser& q, const char* name, size_t name_len)
{
	const char* ni = q.query.data();
	const char* nf = ni + q.query.size();
	const char *n0, *n1 = nullptr;
	const char* v0 = nullptr;

	if (q.off == nullptr) {
		n0 = n1 = ni;
	} else {
		n0 = n1 = q.off + q.len;
	}

	if (n1 > nf) {
		return -1;
	}

	while (true) {
		char cn = (n1 == nf) ? '\0' : *n1;
		switch (cn) {
			case '\1':
				v0 = n1;
				[[fallthrough]];
			case '\0':
				if (name_len == static_cast<size_t>(n1 - n0) && std::strncmp(n0, name, n1 - n0) == 0) {
					if (v0 != nullptr) {
						const char* v1 = v0 + 1;
						while (true) {
							char cv = (v1 == nf) ? '\0' : *v1;
							switch (cv) {
								case '\0':
									q.off = v0 + 1;
									q.len = v1 - v0 - 1;
									return 0;
							}
							++v1;
						}
					} else {
						q.off = n1 + 1;
						q.len = 0;
						return 0;
					}
				} else if (n1 == nf) {
					return -1;
				} else if (cn != '\1') {
					n0 = n1 + 1;
					v0 = nullptr;
				}
		}
		++n1;
	}
	return -1;
}

// PathParser::next, byte by byte.
static PathParser::State
ref_path_next(PathParser& p)
{
	using State = PathParser::State;
	char cn;
	size_t length;
	const char* ni = p.path.data();
	const char* nf = ni + p.path.size();
	const char *n0, *n1 = nullptr;
	State state;

	state = State::PTH;
	p.off_hst = nullptr;
	n0 = n1 = p.off;
	if (p.off_cmd != nullptr && p.off_cmd < nf) { nf = p.off_cmd - 1; }
	if (p.off_slc != nullptr && p.off_slc < nf) { nf = p.off_slc - 1; }
	if (p.off_id != nullptr && p.off_id < nf) { nf = p.off_id - 1; }
	if (nf < ni) { nf = ni; }
	if (n1 > nf) { return State::END; }

	cn = '\xff';
	while (true) {
		cn = (n1 >= nf || n1 < ni) ? '\0' : *n1;
		switch (cn) {
			case '\0':
			case ',':
				switch (state) {
					case State::PTH:
						length = n1 - n0;
						p.off_pth = n0;
						p.len_pth = length;
						p.off = ++n1;
						return state;
					case State::HST:
						length = n1 - n0;
						if (length == 0u) { return State::INVALID_HST; }
						p.off_hst = n0;
						p.len_hst = length;
						p.off = ++n1;
						return state;
					default:
						break;
				}
				break;
			case '@':
				switch (state) {
					case State::PTH:
						length = n1 - n0;
						p.off_pth = n0;
						p.len_pth = length;
						state = State::HST;
						n0 = n1 + 1;
						break;
					default:
						break;
				}
				break;
			default:
				break;
		}
		++n1;
	}
	return state;
}

static long calls = 0;

// A buffer of `len` bytes: runs of letters of random length (some longer than a 64-byte step)
// between delimiters drawn from `delims`, with a delimiter forced at `at` when it is below `len`.
static std::string
make_buffer(std::mt19937_64& rng, size_t len, const std::string& delims, size_t at)
{
	std::string s;
	while (s.size() < len) {
		const size_t run = (rng() % 4 == 0) ? rng() % 150 : rng() % 12;
		for (size_t i = 0; i < run && s.size() < len; ++i) { s += static_cast<char>('a' + rng() % 6); }
		if (s.size() < len) { s += delims[rng() % delims.size()]; }
	}
	s.resize(len);
	if (at < len) { s[at] = delims[rng() % delims.size()]; }
	return s;
}

static void
check_query(const std::string& buf, std::mt19937_64& rng)
{
	QueryParser q;
	q.query = buf;
	q.query_decoded = buf;
	// Names: fields of the buffer, a few random ones, one longer than the buffer, the empty name.
	std::vector<std::string> names = {"", "a", "ab", "k", std::string(buf.size() + 3, 'a')};
	for (int i = 0; i < 4 && !buf.empty(); ++i) {
		const size_t a = rng() % buf.size();
		const size_t b = a + rng() % (buf.size() - a + 1);
		names.push_back(buf.substr(a, b - a));
	}
	for (const auto& name : names) {
		q.off = nullptr;
		q.len = 0;
		for (int step = 0; step < 4; ++step) {
			const char* off0 = q.off;
			const size_t len0 = q.len;
			const int got = q.next(name.data(), name.size());
			const char* off1 = q.off;
			const size_t len1 = q.len;
			q.off = off0;
			q.len = len0;
			const int want = ref_query_next(q, name.data(), name.size());
			CHECK(got == want);
			CHECK(q.off == off1 && q.len == len1);
			++calls;
			if (want != 0) { break; }
		}
	}
}

static void
check_path(const std::string& raw)
{
	PathParser p;
	p.init(raw);
	for (int step = 0; step < 6; ++step) {
		const char* off0 = p.off;
		const char* hst0 = p.off_hst;
		const size_t lh0 = p.len_hst;
		const char* pth0 = p.off_pth;
		const size_t lp0 = p.len_pth;
		const auto got = p.next();
		const char* off1 = p.off;
		const char* hst1 = p.off_hst;
		const size_t lh1 = p.len_hst;
		const char* pth1 = p.off_pth;
		const size_t lp1 = p.len_pth;
		p.off = off0;
		p.off_hst = hst0;
		p.len_hst = lh0;
		p.off_pth = pth0;
		p.len_pth = lp0;
		const auto want = ref_path_next(p);
		CHECK(got == want);
		CHECK(p.off == off1 && p.off_hst == hst1 && p.len_hst == lh1 && p.off_pth == pth1 && p.len_pth == lp1);
		++calls;
		if (want == PathParser::State::END || want >= PathParser::State::INVALID_STATE) { break; }
	}
}

int main() {
	std::mt19937_64 rng(11);
	// Query buffers as the parser builds them: '\0' ends a field, '\1' separates key and value.
	for (size_t len = 0; len <= 300; ++len) {
		for (int rep = 0; rep < 12; ++rep) {
			check_query(make_buffer(rng, len, std::string("\0\1\1", 3), rng() % (len + 1)), rng);
		}
		// Only a long field, then one delimiter at every offset of the steps around 32 and 64.
		for (size_t at : {size_t{0}, size_t{15}, size_t{16}, size_t{31}, size_t{32}, size_t{33}, size_t{63}, size_t{64}, size_t{65}, size_t{127}, size_t{128}}) {
			std::string s(len, 'k');
			if (at < len) { s[at] = (at & 1) ? '\1' : '\0'; }
			check_query(s, rng);
		}
	}
	// Paths: '@' starts a host, ',' ends one, '/' and letters between, a zero byte now and then.
	for (size_t len = 0; len <= 300; ++len) {
		for (int rep = 0; rep < 16; ++rep) {
			std::string s = "/" + make_buffer(rng, len, std::string("/@,/", 4), rng() % (len + 1));
			check_path(s);
		}
		for (size_t at : {size_t{0}, size_t{15}, size_t{31}, size_t{32}, size_t{33}, size_t{63}, size_t{64}, size_t{65}, size_t{127}, size_t{128}}) {
			std::string s = "/" + std::string(len, 'p');
			if (at + 1 < s.size()) { s[at + 1] = (at & 1) ? '@' : ','; }
			check_path(s);
			if (at + 1 < s.size()) { s[at + 1] = '@'; if (at + 5 < s.size()) { s[at + 5] = ','; } check_path(s); }
		}
	}
	std::printf("next() against the byte loops OK: %ld calls\n", calls);
	return 0;
}
