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
// On x86-64 with GCC or clang a long clean stretch is skipped 32 bytes per vector when the CPU has
// AVX2, chosen at run time; URL_PARSER_NO_AVX2 leaves it out (the test builds that too).
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__)) && !defined(URL_PARSER_NO_AVX2)
#include <immintrin.h>
#define URL_PARSER_AVX2 1
#endif
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

// The bytes the decoder must look at: '%' and '+' always, plus '&' ';' '=' when `N` is 5
// (they matter only if their substitutes differ from themselves).
template <int N> struct Needles;
template <> struct Needles<2> { static constexpr char v[2] = {'%', '+'}; };
template <> struct Needles<5> { static constexpr char v[5] = {'%', '+', '&', ';', '='}; };

// A 256-entry table: nonzero where the byte is a needle (the scalar test), and a window
// `t[byte - lo]` over the needles' range (the vector table lookup).
template <int N>
struct NeedleTable {
	unsigned char cls[256];
	unsigned char t[32];
	unsigned char lo, hi;
	constexpr NeedleTable() : cls(), t(), lo(255), hi(0) {
		for (int i = 0; i < N; ++i) {
			unsigned char c = static_cast<unsigned char>(Needles<N>::v[i]);
			cls[c] = 0xff;
			if (c < lo) { lo = c; }
			if (c > hi) { hi = c; }
		}
		for (int i = 0; i < N; ++i) { t[static_cast<unsigned char>(Needles<N>::v[i]) - lo] = 0xff; }
	}
};
template <int N> inline constexpr NeedleTable<N> needle_table;

#if defined(URL_PARSER_AVX2)
// The vectors the AVX2 skip tests: for two needles (byte | (a ^ b)) == (a | b) holds for both and
// for a few other bytes, so it never misses one; for more needles each vector is tested exactly.
template <int N>
__attribute__((target("avx2"), always_inline)) inline __m256i
maybe_avx2(__m256i x) noexcept
{
	constexpr const char (&set)[N] = Needles<N>::v;
	if constexpr (N == 2) {
		return _mm256_cmpeq_epi8(_mm256_or_si256(x, _mm256_set1_epi8(static_cast<char>(set[0] ^ set[1]))), _mm256_set1_epi8(static_cast<char>(set[0] | set[1])));
	} else {
		__m256i h = _mm256_cmpeq_epi8(x, _mm256_set1_epi8(set[0]));
		for (int i = 1; i < N; ++i) { h = _mm256_or_si256(h, _mm256_cmpeq_epi8(x, _mm256_set1_epi8(set[i]))); }
		return h;
	}
}

// Moves `p` over 128-byte steps, then 32-byte ones, that hold no needle and returns where the
// first step that may hold one starts (or where fewer than 32 bytes remain); the caller then finds
// the needle exactly.
template <int N>
__attribute__((target("avx2"))) const char*
skip_clean_avx2(const char* p, const char* end) noexcept
{
	while (end - p >= 128) {
		const __m256i* q = reinterpret_cast<const __m256i*>(p);
		const __m256i h0 = maybe_avx2<N>(_mm256_loadu_si256(q)), h1 = maybe_avx2<N>(_mm256_loadu_si256(q + 1));
		const __m256i h2 = maybe_avx2<N>(_mm256_loadu_si256(q + 2)), h3 = maybe_avx2<N>(_mm256_loadu_si256(q + 3));
		if (_mm256_movemask_epi8(_mm256_or_si256(_mm256_or_si256(h0, h1), _mm256_or_si256(h2, h3)))) { break; }
		p += 128;
	}
	while (end - p >= 32) {
		if (_mm256_movemask_epi8(maybe_avx2<N>(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p))))) { break; }
		p += 32;
	}
	return p;
}

// Whether the CPU (and the OS) supports AVX2, looked up once.
inline bool
has_avx2() noexcept
{
	static const bool yes = (__builtin_cpu_init(), __builtin_cpu_supports("avx2") != 0);
	return yes;
}
#endif

// The index of the first needle among the 8 bytes at `p`, or 8, by an exact 64-bit test: the low
// seven bits of each byte are added without carrying into the next byte, so bit 7 of a lane is
// set exactly where that byte equals the needle (no false positive above a true one).
template <int N>
inline size_t
swar_first8(const char* p) noexcept
{
	std::uint64_t v;
	std::memcpy(&v, p, 8);
	constexpr std::uint64_t lo7 = 0x7f7f7f7f7f7f7f7fULL;
	std::uint64_t h = 0;
	for (int i = 0; i < N; ++i) {
		std::uint64_t x = v ^ (0x0101010101010101ULL * static_cast<unsigned char>(Needles<N>::v[i]));
		h |= ~(((x & lo7) + lo7) | x | lo7);
	}
	if (!h) { return 8; }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	return static_cast<size_t>(__builtin_ctzll(h)) >> 3;
#else
	for (size_t k = 0; k < 8; ++k) { if (needle_table<N>.cls[static_cast<unsigned char>(p[k])] != 0) { return k; } }
	return 8;
#endif
}

// `first(p)` examines W bytes at `p` and returns the index of the first needle, or W if
// there is none; `find(p, end)` returns the first needle of [p, end), or end. NEON and SSE2
// compare 16 bytes at once (`find` takes 64 per step, and on x86-64 with AVX2 skips a long clean
// stretch 128 per step); the portable fallback is a 64-bit SWAR test.
template <int N>
struct Scanner {
	static constexpr const char (&set)[N] = Needles<N>::v;
	static bool is_special(char c) noexcept { return needle_table<N>.cls[static_cast<unsigned char>(c)] != 0; }

#if !defined(URL_PARSER_NO_SIMD) && defined(__ARM_NEON) && defined(__aarch64__)
	static constexpr size_t W = 16;
	static uint8x16_t hits(uint8x16_t v) noexcept {
		constexpr auto& nt = needle_table<N>;
		static_assert(nt.hi - nt.lo < 32, "the needles must fit one table lookup");
		const uint8x16_t idx = vsubq_u8(v, vdupq_n_u8(nt.lo));
		if constexpr (nt.hi - nt.lo < 16) { return vqtbl1q_u8(vld1q_u8(nt.t), idx); }
		else { return vqtbl2q_u8(vld1q_u8_x2(nt.t), idx); }
	}
	// One nibble per byte: the lowest set nibble is the first hit, 16 if none.
	static size_t index(uint8x16_t h) noexcept {
		std::uint64_t m = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(h), 4)), 0);
		return m ? static_cast<size_t>(__builtin_ctzll(m)) >> 2 : 16;
	}
	static size_t first(const char* p) noexcept { return index(hits(vld1q_u8(reinterpret_cast<const uint8_t*>(p)))); }
	// One bit per byte (bit 4 * i for byte i), set where the byte is a needle.
	static std::uint64_t mask(const char* p) noexcept {
		uint8x16_t h = hits(vld1q_u8(reinterpret_cast<const uint8_t*>(p)));
		return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(h), 4)), 0) & 0x8888888888888888ULL;
	}
	static size_t pos(std::uint64_t m) noexcept { return static_cast<size_t>(__builtin_ctzll(m)) >> 2; }
	static const char* find(const char* p, const char* end) noexcept {
		// One vector first: a byte to decode near the start costs one test, not four.
		if (end - p >= 16) {
			size_t k = first(p);
			if (k != 16) { return p + k; }
			p += 16;
		}
		while (end - p >= 64) {
			const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
			const uint8x16_t h0 = hits(vld1q_u8(q)), h1 = hits(vld1q_u8(q + 16));
			const uint8x16_t h2 = hits(vld1q_u8(q + 32)), h3 = hits(vld1q_u8(q + 48));
			if (vmaxvq_u8(vorrq_u8(vorrq_u8(h0, h1), vorrq_u8(h2, h3)))) {
				size_t k;
				if ((k = index(h0)) != 16) { return p + k; }
				if ((k = index(h1)) != 16) { return p + 16 + k; }
				if ((k = index(h2)) != 16) { return p + 32 + k; }
				return p + 48 + index(h3);
			}
			p += 64;
		}
		while (end - p >= 16) {
			size_t k = first(p);
			if (k != 16) { return p + k; }
			p += 16;
		}
		for (; p != end; ++p) { if (is_special(*p)) { return p; } }
		return end;
	}
#elif !defined(URL_PARSER_NO_SIMD) && defined(__SSE2__)
	static constexpr size_t W = 16;
	static __m128i exact(__m128i v) noexcept {
		__m128i h = _mm_cmpeq_epi8(v, _mm_set1_epi8(set[0]));
		for (int i = 1; i < N; ++i) { h = _mm_or_si128(h, _mm_cmpeq_epi8(v, _mm_set1_epi8(set[i]))); }
		return h;
	}
	static size_t index(__m128i h) noexcept {
		unsigned m = static_cast<unsigned>(_mm_movemask_epi8(h));
		return m ? static_cast<size_t>(__builtin_ctz(m)) : 16;
	}
	static size_t first(const char* p) noexcept { return index(exact(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)))); }
	// One bit per byte (bit i for byte i), set where the byte is a needle.
	static std::uint64_t mask(const char* p) noexcept {
		return static_cast<unsigned>(_mm_movemask_epi8(exact(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)))));
	}
	static size_t pos(std::uint64_t m) noexcept { return static_cast<size_t>(__builtin_ctzll(m)); }
	// The vectors `find` tests: for two needles (byte | (a ^ b)) == (a | b) holds
	// for both and for a few other bytes, so one or and one compare per vector find the blocks
	// worth an exact test; for more needles each vector is tested exactly.
	static __m128i maybe(__m128i x) noexcept {
		if constexpr (N == 2) {
			return _mm_cmpeq_epi8(_mm_or_si128(x, _mm_set1_epi8(static_cast<char>(set[0] ^ set[1]))), _mm_set1_epi8(static_cast<char>(set[0] | set[1])));
		} else {
			return exact(x);
		}
	}
	static const char* find(const char* p, const char* end) noexcept {
		// One vector first: a byte to decode near the start costs one test, not four.
		if (end - p >= 16) {
			size_t k = first(p);
			if (k != 16) { return p + k; }
			p += 16;
		}
#if defined(URL_PARSER_AVX2)
		if (end - p >= 256 && has_avx2()) { p = skip_clean_avx2<N>(p, end); }
#endif
		while (end - p >= 64) {
			const __m128i* q = reinterpret_cast<const __m128i*>(p);
			const __m128i x0 = _mm_loadu_si128(q), x1 = _mm_loadu_si128(q + 1);
			const __m128i x2 = _mm_loadu_si128(q + 2), x3 = _mm_loadu_si128(q + 3);
			const __m128i h0 = maybe(x0), h1 = maybe(x1), h2 = maybe(x2), h3 = maybe(x3);
			if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(h0, h1), _mm_or_si128(h2, h3)))) {
				size_t k;
				if ((k = index(exact(x0))) != 16) { return p + k; }
				if ((k = index(exact(x1))) != 16) { return p + 16 + k; }
				if ((k = index(exact(x2))) != 16) { return p + 32 + k; }
				if ((k = index(exact(x3))) != 16) { return p + 48 + k; }
			}
			p += 64;
		}
		while (end - p >= 16) {
			size_t k = first(p);
			if (k != 16) { return p + k; }
			p += 16;
		}
		for (; p != end; ++p) { if (is_special(*p)) { return p; } }
		return end;
	}
#else
	static constexpr size_t W = 8;
	static size_t first(const char* p) noexcept { return swar_first8<N>(p); }
	static const char* find(const char* p, const char* end) noexcept {
		while (end - p >= static_cast<std::ptrdiff_t>(W)) {
			size_t k = first(p);
			if (k != W) { return p + k; }
			p += W;
		}
		for (; p != end; ++p) { if (is_special(*p)) { return p; } }
		return end;
	}
#endif
};

// The output byte for the special byte at `s`: the cursor `p` moves past it (and past the two
// hex digits of a valid escape). `plain` is the byte before the separators are mapped.
__attribute__((always_inline)) inline char
decode_one(const char* s, const char*& p, const char* end, const Subst& m, char& plain) noexcept
{
	p = s + 1;
	char c = *s;
	plain = c;
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
	return subst(c, m);
}

// Decodes the special byte at `s` as `decode_one` does and writes its output byte at `o`
// (and, when `Dual`, the plain decoding at the same offset from `o2`).
template <bool Dual>
__attribute__((always_inline)) inline void
decode_special(const char* s, const char*& p, const char* end, const Subst& m, char*& o, const char* o1, char* o2) noexcept
{
	char plain;
	const char c = decode_one(s, p, end, m, plain);
	if (Dual) { o2[o - o1] = subst(plain, Subst{' ', '&', ';', '=', '\0'}); }
	*o++ = c;
}

// How many bytes of one call the mask pass of `decode_core` takes. On arm64 it takes them all. On
// x86-64 it takes the first 4 KB: its loop over the specials of a block ends after a count that
// varies from block to block, and on Intel cores those exits mispredict once the input is too
// long for the branch predictor to remember, which makes it slower than the block loop on long
// escape-dense input. Up to 4 KB the mask pass is the faster of the two.
#if defined(__aarch64__)
constexpr size_t mask_limit = static_cast<size_t>(-1);
#else
constexpr size_t mask_limit = 4096;
#endif

// GCC keeps `decode_core` out of line: inlined into the parsers' `init` it slowed `QueryParser`
// by a quarter (x86-64, GCC 13). Clang is faster when it decides for itself (Apple clang, x86-64
// and arm64), so it gets no attribute.
#if defined(__clang__)
#define URL_PARSER_CORE_ATTR
#else
#define URL_PARSER_CORE_ATTR __attribute__((noinline))
#endif

// One pass over [p, end), the decoded bytes written through `o1` and, when `Dual`,
// the bytes of the plain decoding (`urldecode(p, size)` defaults) through `o2`; the
// two outputs always have the same length, which is returned. Blocks of clean bytes
// are copied wholesale while they are scanned: a block is stored first, then the
// output advances only past the bytes before the first special one. `o1` (and `o2`)
// must have room for the whole input, which also covers the over-copy (output never
// outruns input).
template <bool Dual, int N>
URL_PARSER_CORE_ATTR size_t
decode_core(const char* p, const char* end, const Subst& m, char* o1, char* o2) noexcept
{
	using scan = Scanner<N>;
	char* o = o1;

	if constexpr (scan::W == 16) {
		// One vector test per block, then each special of the block from its bit: no rescan
		// per special. A decoded escape consumes two hex digits, which are never needles, so
		// the bits of the block stay valid. Runs of clean bytes are stored 16 at a time; a
		// run is shorter than a block, and 32 bytes left leave room for that over-copy. The
		// pass takes at most `mask_limit` bytes of a call; the block loop below takes the rest.
		const char* const mask_stop = static_cast<size_t>(end - p) > mask_limit ? p + mask_limit : end;
		while (end - p >= 32 && p < mask_stop) {
			std::uint64_t mm = scan::mask(p);
			if (!mm) {
				std::memcpy(o, p, 16);
				if (Dual) { std::memcpy(o2 + (o - o1), p, 16); }
				p += 16;
				o += 16;
				continue;
			}
			const char* block = p;
			const char* src = p;
			do {
				const char* s = block + scan::pos(mm);
				mm &= mm - 1;
				const size_t run = static_cast<size_t>(s - src);
				std::memcpy(o, src, 16);
				if (Dual) { std::memcpy(o2 + (o - o1), src, 16); }
				o += run;
				decode_special<Dual>(s, p, end, m, o, o1, o2);
				src = p;
			} while (mm);
			if (src < block + 16) {
				std::memcpy(o, src, 16);
				if (Dual) { std::memcpy(o2 + (o - o1), src, 16); }
				o += block + 16 - src;
				p = block + 16;
			}
		}
	}

	for (;;) {
		const char* s;
		if (static_cast<size_t>(end - p) >= Scanner<N>::W) {
			size_t k = scan::first(p);
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
			while (s != end && !scan::is_special(*s)) {
				*o = *s;
				if (Dual) { o2[o - o1] = *s; }
				++o;
				++s;
			}
			if (s == end) { break; }
		}
		decode_special<Dual>(s, p, end, m, o, o1, o2);
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

// Inputs of at least `stack_min` and at most `stack_max` bytes are decoded into a stack buffer
// and copied once into a string of the exact size: for these the string's own resize calls (a
// zero-fill, then a shrink) cost more than the copy. Larger inputs are decoded in place. On
// arm64 inputs under 48 bytes are decoded in place too: there the stack round trip (stores, then
// a copy that reads them back) loses to the in-place decode on 16 to 47 bytes with a few
// escapes, while on x86-64 it wins from the first byte past one block.
#if defined(__aarch64__)
constexpr size_t stack_min = 48;
// Whether inputs of 8 to 15 bytes are scanned two words at a time (x86-64) or by the byte loop
// (arm64, where the byte loop is cheap and the word test costs more when the first special is
// near the start).
constexpr bool small_words = false;
#else
constexpr size_t stack_min = 17;
constexpr bool small_words = true;
#endif
constexpr size_t stack_max = 512;

// Decodes [first, end), where `first` is the first special byte of the input starting at `q`,
// behind a copy of the clean bytes before it. Out of line, so that the calls that return early
// do not set up a 512-byte frame.
template <int N>
__attribute__((noinline)) std::string
decode_on_stack(const char* q, const char* first, const char* end, const Subst& m)
{
	char tmp[stack_max];
	const size_t head = static_cast<size_t>(first - q);
	// The input has at least 17 bytes here: the first 16 are copied whole, the rest of the head by length.
	std::memcpy(tmp, q, 16);
	if (head > 16) { std::memcpy(tmp + 16, q + 16, head - 16); }
	return std::string(tmp, head + decode_core<false, N>(first, end, m, tmp + head, nullptr));
}

template <int N>
std::string
decode_string(const char* q, const char* end, const Subst& m)
{
	const size_t size = static_cast<size_t>(end - q);
	// Shorter than one block: from 8 bytes two 8-byte words look for the first byte to decode;
	// below that one byte loop copies the clean prefix while it looks. The rest is decoded into a
	// small buffer.
	if (size < Scanner<N>::W) {
		if constexpr (Scanner<N>::W > 8 && small_words) {
			if (size >= 8) {
				// Two 8-byte words (overlapping when the input is shorter than 16) cover the input.
				size_t k = swar_first8<N>(q);
				const char* first = q + k;
				if (k == 8) {
					const size_t k2 = swar_first8<N>(end - 8);
					if (k2 == 8) { return std::string(q, size); }
					first = end - 8 + k2;
				}
				char tmp[Scanner<N>::W];
				const size_t head = static_cast<size_t>(first - q);
				// The clean head, with fixed-size copies (a call to memcpy costs more than the rest).
				std::memcpy(tmp, q, 8);
				if (head > 8) { std::memcpy(tmp + head - 8, q + head - 8, 8); }
				return std::string(tmp, head + decode_core<false, N>(first, end, m, tmp + head, nullptr));
			}
		}
		char tmp[Scanner<N>::W];
		char* o = tmp;
		const char* s = q;
		while (s != end && !Scanner<N>::is_special(*s)) { *o++ = *s++; }
		if (s == end) { return std::string(q, size); }
		return std::string(tmp, static_cast<size_t>(o - tmp) + decode_core<false, N>(s, end, m, o, nullptr));
	}
	const char* first = Scanner<N>::find(q, end);
	// Clean input (nothing to decode or map): one copy, no zero-fill.
	if (first == end) { return std::string(q, size); }
	if (size == Scanner<N>::W) {
		// One block, and a byte to decode in it: no function call, no large frame.
		char tmp[Scanner<N>::W];
		const size_t head = static_cast<size_t>(first - q);
		std::memcpy(tmp, q, Scanner<N>::W);
		return std::string(tmp, head + decode_core<false, N>(first, end, m, tmp + head, nullptr));
	}
	if (size >= stack_min && size <= stack_max) { return decode_on_stack<N>(q, first, end, m); }
	const size_t head = static_cast<size_t>(first - q);
	std::string buf;
	fill(buf, size, [&](char* d) {
		std::memcpy(d, q, head);
		return head + decode_core<false, N>(first, end, m, d + head, nullptr);
	});
	return buf;
}

}  // namespace


std::string
urldecode(const void *p, size_t size, char plus, char amp, char colon, char eq, char encoded)
{
	if (size == 0) { return std::string(); }
	const Subst m{plus, amp, colon, eq, encoded};
	const auto* q = static_cast<const char *>(p);
	if (mapped_separators(m)) { return decode_string<5>(q, q + size, m); }
	return decode_string<2>(q, q + size, m);
}


// Fills `a` with urldecode(p, size, m...) and `b` with urldecode(p, size), in one pass.
static void
urldecode2(const char* p, size_t size, const Subst& m, std::string& a, std::string& b)
{
	a.resize(size);
	b.resize(size);
	size_t n = size ? decode_core<true, 5>(p, p + size, m, &a[0], &b[0]) : 0;
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
