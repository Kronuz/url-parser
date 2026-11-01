// Timing guards for two measured properties of urldecode, so a change that loses them fails a
// test rather than a benchmark nobody runs:
//   1. Short input (4 to 16 bytes, with escapes) costs no more than the byte-at-a-time
//      reference decoder: the scanner's set-up and the string's own resize calls used to cost
//      more than the decoding.
//   2. The scan for the first byte to decode takes several vectors per step: it is faster than
//      the one-vector loop built from the same per-vector test.
// Each side is timed in alternating rounds and the best round of each is compared, so load
// on the machine inflates both and the ratio stays; a failing ratio is measured again before
// it is reported. Skipped (exit status 77) where timing means nothing: unoptimized builds
// and sanitizers.
// The guard compares two decoders by speed, not the standard library's checks around them: a
// hardened libc++ (as the CI builds it) adds bounds checks to the std::string operations of one
// side more than the other, which moves the short-input ratio by 2x. The checks stay on in the
// other tests.
#ifdef _LIBCPP_HARDENING_MODE
#undef _LIBCPP_HARDENING_MODE
#endif
#define _LIBCPP_HARDENING_MODE _LIBCPP_HARDENING_MODE_NONE

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Compiled together with the implementation to reach the scanner.
#include "../url_parser.cc"
#include "urldecode_old.h"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(__OPTIMIZE__)
#define TIMING_MEANINGLESS 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define TIMING_MEANINGLESS 1
#endif
#endif

using clk = std::chrono::steady_clock;

template <typename T>
static inline void use(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }

// Best (lowest) ns per call over `rounds`, for two callables run in alternating rounds.
template <typename A, typename B>
static void best_of(A&& a, B&& b, int rounds, std::uint64_t calls, double& best_a, double& best_b) {
	best_a = best_b = 1e30;
	for (int r = 0; r < rounds; ++r) {
		auto t0 = clk::now();
		for (std::uint64_t i = 0; i < calls; ++i) { a(); }
		auto t1 = clk::now();
		for (std::uint64_t i = 0; i < calls; ++i) { b(); }
		auto t2 = clk::now();
		best_a = std::min(best_a, std::chrono::duration<double, std::nano>(t1 - t0).count() / double(calls));
		best_b = std::min(best_b, std::chrono::duration<double, std::nano>(t2 - t1).count() / double(calls));
	}
}

int main() {
#ifdef TIMING_MEANINGLESS
	std::printf("url_parser perf guard: skipped (unoptimized or sanitized build)\n");
	return 77;
#else
	bool ok = true;

	// 1. Short input with escapes: urldecode against the reference decoder.
	{
		const std::vector<std::string> inputs = {"a%20b", "x+y", "%41%42", "ab%2Bc+d", "k=v&a", "%zz", "abc%", "+", "q=%7Bx%7D", "12345678%41", "a+b%20c+d%2B", "%41%42%43%44%45"};
		double ratio = 0;
		for (int attempt = 0; attempt < 3; ++attempt) {
			double now = 0, old = 0;
			for (const auto& in : inputs) {
				double a, b;
				best_of([&] { auto s = urldecode(in); use(s); },
				        [&] { auto s = old_urldecode(in.data(), in.size()); use(s); }, 25, 20000, a, b);
				now += a;
				old += b;
			}
			ratio = now / old;
			if (ratio <= 1.15) { break; }
		}
		std::printf("short input: urldecode / reference = %.2f (limit 1.15)\n", ratio);
		if (ratio > 1.15) { std::fprintf(stderr, "FAIL: urldecode is slower than the byte-at-a-time decoder on short input\n"); ok = false; }
	}

	// 2. The scan: several vectors per step against one.
	if (Scanner<2>::W == 16) {
		std::string buf(4096, 'x');
		for (size_t i = 0; i < buf.size(); ++i) { buf[i] = char('a' + i % 26); }
		const char* p = buf.data();
		const char* e = p + buf.size();
		auto narrow = [&] {
			const char* q = p;
			while (e - q >= 16) { size_t k = Scanner<2>::first(q); if (k != 16) { break; } q += 16; }
			return q;
		};
		double ratio = 0;
		for (int attempt = 0; attempt < 3; ++attempt) {
			double a, b;
			best_of([&] { const char* r = Scanner<2>::find(p, e); use(r); }, [&] { const char* r = narrow(); use(r); }, 25, 4000, a, b);
			ratio = b / a;
			if (ratio >= 1.30) { break; }
		}
		std::printf("scan: one vector per step / several = %.2f (floor 1.30)\n", ratio);
		if (ratio < 1.30) { std::fprintf(stderr, "FAIL: the scan is not faster than one vector per step\n"); ok = false; }
	}

	std::printf(ok ? "url_parser perf guard OK\n" : "url_parser perf guard FAILED\n");
	return ok ? 0 : 1;
#endif
}
