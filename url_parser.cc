/*
 * Copyright © 2026 Germán Méndez Bravo (Kronuz)
 * Copyright (c) 2015-2019 Dubalu LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "url_parser.h"

#include <cassert>                                // for assert
#include <cstdint>                                // for std::uint64_t
#include <cstring>                                // for std::strncmp, std::memcpy

// URL_PARSER_NO_SIMD forces the portable SWAR scanner (the test builds it too).
#if defined(URL_PARSER_NO_SIMD)
#elif defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif


namespace {
// normalize_path is copied verbatim from Xapiand's src/fs.cc (Copyright (c)
// 2015-2019 Dubalu LLC, MIT). Despite living next to filesystem helpers there,
// it is pure string manipulation: it collapses "..", "//", and trailing slashes
// in a single pass over the buffer, touching nothing on disk. It is the only
// thing url_parser needed from "fs.hh", so it is inlined here to keep this
// library free of a filesystem dependency.

size_t
normalize_path(const char* src, const char* end, char* dst, bool slashed, bool keep_slash)
{
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


std::string
normalize_path(std::string_view src, bool slashed, bool keep_slash)
{
	size_t src_size = src.size();
	const char* src_str = src.data();
	std::string dst;
	dst.resize(src_size + 1);
	auto dst_size = normalize_path(src_str, src_str + src_size, &dst[0], slashed, keep_slash);
	dst.resize(dst_size);
	return dst;
}
}  // namespace


namespace {

// Nibble value of an ASCII hex digit, 0xff for any other byte.
struct HexTable {
	unsigned char v[256];
	constexpr HexTable() : v() {
		for (int i = 0; i < 256; ++i) { v[i] = 0xff; }
		for (int i = 0; i < 10; ++i) { v['0' + i] = static_cast<unsigned char>(i); }
		for (int i = 0; i < 6; ++i) { v['a' + i] = v['A' + i] = static_cast<unsigned char>(10 + i); }
	}
};
constexpr HexTable hex_table;

struct Subst {
	char plus, amp, colon, eq, encoded;
};

// The byte written for a (literal or decoded) byte `c`: the four separator
// characters are replaced by their substitutes, everything else is verbatim.
inline char
subst(char c, const Subst& m) noexcept
{
	switch (c) {
		case '+': return m.plus;
		case '&': return m.amp;
		case ';': return m.colon;
		case '=': return m.eq;
		default: return c;
	}
}

// Finds the first byte of a block that the decoder must look at: '%' and '+' always,
// plus '&' ';' '=' when `N` is 5 (they matter only if their substitutes differ from
// themselves). `first(p)` examines W bytes at `p` and returns the index of the first
// such byte, or W if there is none. NEON and SSE2 compare 16 bytes at once; the
// portable fallback is a 64-bit SWAR test.
template <int N>
struct Scanner {
	const char (&set)[N];
#if !defined(URL_PARSER_NO_SIMD) && defined(__ARM_NEON) && defined(__aarch64__)
	static constexpr size_t W = 16;
	uint8x16_t rep[N];
	explicit Scanner(const char (&s)[N]) : set(s) {
		for (int i = 0; i < N; ++i) { rep[i] = vdupq_n_u8(static_cast<uint8_t>(s[i])); }
	}
	size_t first(const char* p) const noexcept {
		uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
		uint8x16_t hit = vceqq_u8(v, rep[0]);
		for (int i = 1; i < N; ++i) { hit = vorrq_u8(hit, vceqq_u8(v, rep[i])); }
		// One nibble per byte: the lowest set nibble is the first hit.
		std::uint64_t m = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(hit), 4)), 0);
		return m ? static_cast<size_t>(__builtin_ctzll(m)) >> 2 : W;
	}
#elif !defined(URL_PARSER_NO_SIMD) && defined(__SSE2__)
	static constexpr size_t W = 16;
	__m128i rep[N];
	explicit Scanner(const char (&s)[N]) : set(s) {
		for (int i = 0; i < N; ++i) { rep[i] = _mm_set1_epi8(s[i]); }
	}
	size_t first(const char* p) const noexcept {
		__m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
		__m128i hit = _mm_cmpeq_epi8(v, rep[0]);
		for (int i = 1; i < N; ++i) { hit = _mm_or_si128(hit, _mm_cmpeq_epi8(v, rep[i])); }
		unsigned m = static_cast<unsigned>(_mm_movemask_epi8(hit));
		return m ? static_cast<size_t>(__builtin_ctz(m)) : W;
	}
#else
	static constexpr size_t W = 8;
	std::uint64_t rep[N];
	explicit Scanner(const char (&s)[N]) : set(s) {
		for (int i = 0; i < N; ++i) { rep[i] = 0x0101010101010101ULL * static_cast<unsigned char>(s[i]); }
	}
	size_t first(const char* p) const noexcept {
		std::uint64_t v;
		std::memcpy(&v, p, 8);
		std::uint64_t h = 0;
		for (int i = 0; i < N; ++i) {
			std::uint64_t x = v ^ rep[i];
			h |= (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
		}
		if (!h) { return W; }
		// Bits above the first zero byte may be false positives, so locate it bytewise.
		for (size_t k = 0; k < W; ++k) {
			for (int i = 0; i < N; ++i) { if (p[k] == set[i]) { return k; } }
		}
		return W;
	}
#endif
	bool is_special(char c) const noexcept {
		for (int i = 0; i < N; ++i) { if (c == set[i]) { return true; } }
		return false;
	}
	// First special byte of [p, end), or end.
	const char* find(const char* p, const char* end) const noexcept {
		while (static_cast<size_t>(end - p) >= W) {
			size_t k = first(p);
			if (k != W) { return p + k; }
			p += W;
		}
		for (; p != end; ++p) { if (is_special(*p)) { return p; } }
		return end;
	}
};

// One pass over [p, end), the decoded bytes written through `o1` and, when `Dual`,
// the bytes of the plain decoding (`urldecode(p, size)` defaults) through `o2`; the
// two outputs always have the same length, which is returned. Blocks of clean bytes
// are copied wholesale while they are scanned: a block is stored first, then the
// output advances only past the bytes before the first special one. `o1` (and `o2`)
// must have room for the whole input, which also covers the over-copy (output never
// outruns input).
template <bool Dual, int N>
size_t
decode_core(const char* p, const char* end, const Subst& m, const char (&set)[N], char* o1, char* o2) noexcept
{
	const Scanner<N> scan(set);
	char* o = o1;
	for (;;) {
		const char* s;
		if (static_cast<size_t>(end - p) >= Scanner<N>::W) {
			size_t k = scan.first(p);
			std::memcpy(o, p, Scanner<N>::W);
			if (Dual) { std::memcpy(o2 + (o - o1), p, Scanner<N>::W); }
			if (k == Scanner<N>::W) {
				p += Scanner<N>::W;
				o += Scanner<N>::W;
				continue;
			}
			s = p + k;
			o += k;
		} else {
			// Fewer than a block left: byte by byte.
			s = p;
			while (s != end && !scan.is_special(*s)) {
				*o = *s;
				if (Dual) { o2[o - o1] = *s; }
				++o;
				++s;
			}
			if (s == end) { break; }
		}
		p = s + 1;
		char c = *s;
		char plain = c;
		if (c == '%') {
			// Only decode when both hex digits are inside the buffer.
			if (end - p >= 2) {
				unsigned hi = hex_table.v[static_cast<unsigned char>(p[0])];
				unsigned lo = hex_table.v[static_cast<unsigned char>(p[1])];
				if ((hi | lo) != 0xff) {
					p += 2;
					c = static_cast<char>(hi << 4 | lo);
					plain = c;
					if (m.encoded && c != '{' && c != ',' && c != '}') { c = m.encoded; }
				}
			}
		}
		if (Dual) { o2[o - o1] = subst(plain, Subst{' ', '&', ';', '=', '\0'}); }
		*o++ = subst(c, m);
		if (p == end) { break; }
	}
	return static_cast<size_t>(o - o1);
}

inline bool
mapped_separators(const Subst& m) noexcept
{
	return m.amp != '&' || m.colon != ';' || m.eq != '=';
}

template <typename F>
inline void
fill(std::string& s, size_t size, F&& f)
{
#if defined(__cpp_lib_string_resize_and_overwrite)
	s.resize_and_overwrite(size, [&](char* d, size_t) { return f(d); });
#else
	s.resize(size);
	s.resize(f(&s[0]));
#endif
}

}  // namespace


std::string
urldecode(const void *p, size_t size, char plus, char amp, char colon, char eq, char encoded)
{
	if (size == 0) { return std::string(); }
	const Subst m{plus, amp, colon, eq, encoded};
	const auto* q = static_cast<const char *>(p);
	const char* end = q + size;
	static constexpr char lean[2] = {'%', '+'};
	static constexpr char full[5] = {'%', '+', '&', ';', '='};
	std::string buf;
	auto run = [&](const auto& scan, const auto& set) {
		const char* first = scan.find(q, end);
		// Clean input (nothing to decode or map): one bulk copy, no zero-fill.
		if (first == end) { buf.assign(q, size); return; }
		fill(buf, size, [&](char* d) {
			std::memcpy(d, q, static_cast<size_t>(first - q));
			return static_cast<size_t>(first - q) + decode_core<false>(first, end, m, set, d + (first - q), nullptr);
		});
	};
	if (mapped_separators(m)) { run(Scanner<5>(full), full); } else { run(Scanner<2>(lean), lean); }
	return buf;
}


// Fills `a` with urldecode(p, size, m...) and `b` with urldecode(p, size), in one pass.
static void
urldecode2(const char* p, size_t size, const Subst& m, std::string& a, std::string& b)
{
	static constexpr char full[5] = {'%', '+', '&', ';', '='};
	a.resize(size);
	b.resize(size);
	size_t n = size ? decode_core<true>(p, p + size, m, full, &a[0], &b[0]) : 0;
	a.resize(n);
	b.resize(n);
}


QueryParser::QueryParser()
	: off(nullptr),
	  len(0) { }


void
QueryParser::clear() noexcept
{
	rewind();
	query.clear();
	query_decoded.clear();
}


void
QueryParser::rewind() noexcept
{
	len = 0;
	off = nullptr;
}


int
QueryParser::init(std::string_view q)
{
	clear();
	urldecode2(q.data(), q.size(), Subst{' ', '\0', '\0', '\1', '%'}, query, query_decoded);
	return 0;
}


int
QueryParser::next(const char *name, size_t name_len)
{
	const char *ni = query.data();
	const char *nf = ni + query.size();
	const char *n0, *n1 = nullptr;
	const char *v0 = nullptr;

	if (off == nullptr) {
		n0 = n1 = ni;
	} else {
		n0 = n1 = off + len;
	}

	if (n1 > nf) {
		return -1;
	}

	while (true) {
		char cn = (n1 == nf) ? '\0' : *n1;
		switch (cn) {
			case '\1':  // '='
				v0 = n1;
				[[fallthrough]];
			case '\0':  // '\0' and '&'
				if (name_len == static_cast<size_t>(n1 - n0) && std::strncmp(n0, name, n1 - n0) == 0) {
					if (v0 != nullptr) {
						const char *v1 = v0 + 1;
						while (true) {
							char cv = (v1 == nf) ? '\0' : *v1;
							switch (cv) {
								case '\0':  // '\0' and '&'
									off = v0 + 1;
									len = v1 - v0 - 1;
									return 0;
							}
							++v1;
						}
					} else {
						off = n1 + 1;
						len = 0;
						return 0;
					}
				} else if (n1 == nf) {
					return -1;
				} else if (cn != '\1') {  // '='
					n0 = n1 + 1;
					v0 = nullptr;
				}
		}
		++n1;
	}

	return -1;
}


std::string_view
QueryParser::get()
{
	if (off == nullptr) { return ""; }
	return std::string_view(off - query.data() + query_decoded.data(), len);
}


PathParser::PathParser()
	: off(nullptr), len_pth(0),
	  off_pth(nullptr), len_hst(0),
	  off_hst(nullptr), len_slc(0),
	  off_slc(nullptr), len_id(0),
	  off_id(nullptr), len_cmd(0),
	  off_cmd(nullptr) { }


void
PathParser::clear() noexcept
{
	rewind();
	path.clear();
	path_decoded.clear();
	off_id = nullptr;
	len_id = 0;
}


void
PathParser::rewind() noexcept
{
	off = path.data();
	len_pth = 0;
	off_pth = nullptr;
	len_hst = 0;
	off_hst = nullptr;
}


PathParser::State
PathParser::init(std::string_view p)
{
	auto np = normalize_path(p, false, true);

	clear();
	urldecode2(np.data(), np.size(), Subst{' ', '&', ';', '=', '%'}, path, path_decoded);

	char cn;
	size_t length;
	const char *ni = path.data();
	const char *nf = ni + path.size();
	const char *n0, *n1 , *ns= nullptr;
	size_t takeoff = 0, addin=0, slc_level = 0;
	State state;

	state = State::CMD;

	off = ni;

	if (path.empty() || path.back() == '/') {
		return State::PTH;
	}

	// Go backwards, looking for id or selector
	ns = n0 = n1 = nf - 1;

	cn = '\xff';
	while (cn != 0) {
		cn = (n1 >= nf || n1 < ni) ? '\0' : *n1;

		switch (cn) {
			case '\0':
			case '/':
				switch (state) {
					case State::CMD:
					case State::SLC:
						length = n0 - n1;
						if (length != 0u) {
							off_id = n1 + 1;
							len_id = length;
						}
						cn = '\0';
						n0 = n1 - 1;
						break;

					case State::ID:
					case State::SLB:
					case State::SLB_SPACE_OR_COMMA:
						assert(n0 >= n1);
						length = n0 - n1;
						if (length != 0u) {
							off_id = n1 + 1;
							len_id = length;
						}
						cn = '\0';
						n0 = n1 - 1;
						break;

					case State::SLF:
					case State::ID_SLC:
						assert(n0 >= n1);
						length = ns - n1 - 1;
						if (length != 0u) {
							off_id = n1 + 1;
							len_id = length;
						}
						length = n0 - ns + addin;
						if (length != 0u) {
							off_slc = ns + takeoff;
							len_slc = length;
						}
						cn = '\0';
						n0 = n1 - 1;
						break;

					default:
						break;
				}
				break;

			case '.':  // Drill Selector
				switch (state) {
					case State::CMD:
					case State::SLC:
					case State::SLB:
					case State::ID_SLC:
						ns = n1;
						takeoff = 1; // Removing the dot
						addin = 0;   // Adding nothing to length
						state = State::SLF;
						break;
					default:
						break;
				}
				break;

			case '}':  // Field Selector
				++slc_level;
				switch (state) {
					case State::CMD:
					case State::SLC:
						state = State::SLB;
						break;
					case State::SLB:
						state = State::SLB_SUB;
						break;
					default:
						break;
				}
				break;

			case '{':  // Field Selector
				--slc_level;
				switch (state) {
					case State::SLB:
						if (slc_level == 0) {
							ns = n1;
							takeoff = 0; // Take off nothing
							addin = 1;   // Adding last bracket to length
							state = State::SLF;
						} else {
							state = State::ID;
						}
						break;
					case State::SLB_SUB:
						state = State::SLB_SPACE_OR_COMMA;
						break;
					default:
						break;
				}
				break;
			case ' ':
				switch (state) {
					case State::SLB_SPACE_OR_COMMA:
						if (slc_level - 1 == 0) {
							state = State::SLB;
						}
						break;
					default:
						break;
				}
				break;
			case ',':
				switch (state) {
					case State::SLC:
						state = State::ID;
						n0 = n1;
						break;
					case State::SLB_SPACE_OR_COMMA:
						if (slc_level - 1 == 0) {
							state = State::SLB;
						}
						break;
					case State::CMD:
						return State::INVALID_STATE;
					default:
						break;
				}
				break;
			case command__:
				switch (state) {
					case State::CMD:
						length = n0 - n1 + 1;
						off_cmd = n1;
						len_cmd = length;
						state = State::SLC;
						n0 = n1 - 1;
						break;
					default:
						break;
				}
				break;
			case '@':
				switch (state) {
					case State::CMD:
					case State::SLC:
						state = State::ID;
						n0 = n1;
						break;
					default:
						break;
				}
				break;

			default:
				switch (state) {
					case State::SLF:
						state = State::ID_SLC;
						break;
					default:
						break;
				}
				break;
		}
		--n1;
	}

	return state;
}


PathParser::State
PathParser::next()
{
	char cn;
	size_t length;
	const char *ni = path.data();
	const char *nf = ni + path.size();
	const char *n0, *n1 = nullptr;
	State state;

	// Then goes forward, looking for endpoints:
	state = State::PTH;
	off_hst = nullptr;
	n0 = n1 = off;
	if (off_cmd != nullptr && off_cmd < nf) {
		nf = off_cmd - 1;
	}
	if (off_slc != nullptr && off_slc < nf) {
		nf = off_slc - 1;
	}
	if (off_id != nullptr && off_id < nf) {
		nf = off_id - 1;
	}
	if (nf < ni) {
		nf = ni;
	}
	if (n1 > nf) {
		return State::END;
	}

	cn = '\xff';
	while (true) {
		cn = (n1 >= nf || n1 < ni) ? '\0' : *n1;

		switch (cn) {
			case '\0':
			case ',':
				switch (state) {
					case State::PTH:
						assert(n1 >= n0);
						length = n1 - n0;
						off_pth = n0;
						len_pth = length;
						off = ++n1;
						return state;
					case State::HST:
						assert(n1 >= n0);
						length = n1 - n0;
						if (length == 0u) {
							return State::INVALID_HST;
						}
						off_hst = n0;
						len_hst = length;
						off = ++n1;
						return state;
					default:
						break;
				}
				break;

			case '@':
				switch (state) {
					case State::PTH:
						assert(n1 >= n0);
						length = n1 - n0;
						off_pth = n0;
						len_pth = length;
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


bool
PathParser::has_pth() noexcept
{
	if (off_cmd != nullptr) {
		return off_cmd > (off + 1);
	}

	if (off_slc != nullptr) {
		if (len_slc > 0 && *off_slc == '{') {
			return off_slc > (off + 1);
		}
		return off_slc > (off + 2);
	}

	if (off_id != nullptr) {
		return off_id > (off + 1);
	}

	return (path.data() + path.size()) > (off + 1);
}


std::string_view
PathParser::get_pth()
{
	if (off_pth == nullptr) { return ""; }
	return std::string_view(off_pth - path.data() + path_decoded.data(), len_pth);
}


std::string_view
PathParser::get_hst()
{
	if (off_hst == nullptr) { return ""; }
	return std::string_view(off_hst - path.data() + path_decoded.data(), len_hst);
}


std::string_view
PathParser::get_id()
{
	if (off_id == nullptr) { return ""; }
	return std::string_view(off_id - path.data() + path_decoded.data(), len_id);
}


std::string_view
PathParser::get_slc()
{
	if (off_slc == nullptr) { return ""; }
	return std::string_view(off_slc - path.data() + path_decoded.data(), len_slc);
}


std::string_view
PathParser::get_cmd()
{
	if (off_cmd == nullptr) { return ""; }
	return std::string_view(off_cmd - path.data() + path_decoded.data(), len_cmd);
}
