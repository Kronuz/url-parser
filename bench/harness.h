// Tiny timing harness: each case is run in `trials` trials of >= ~target_ms; reports the
// median ns/op and the spread (max-min)/median across trials. Pass the same lambda twice
// (labels "x#1", "x#2") to get an A/A spread.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace bh {
using clk = std::chrono::steady_clock;

template <typename F>
inline void keep(F&& f) { asm volatile("" ::: "memory"); (void)f; }
template <typename T>
inline void use(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }

struct Result { double med_ns, spread; };

// f(n) runs the operation n times and returns nothing; returns ns per op.
template <typename F>
Result measure(F&& f, int trials = 9, double target_ms = 40) {
	// calibrate
	uint64_t n = 1;
	for (;;) {
		auto t0 = clk::now();
		f(n);
		double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
		if (ms >= target_ms / 4 || n > (1ull << 32)) { n = std::max<uint64_t>(1, uint64_t(n * (target_ms / std::max(ms, 0.001)))); break; }
		n *= 4;
	}
	std::vector<double> v;
	for (int t = 0; t < trials; ++t) {
		auto t0 = clk::now();
		f(n);
		double ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
		v.push_back(ns / double(n));
	}
	std::sort(v.begin(), v.end());
	double med = v[v.size() / 2];
	return {med, (v.back() - v.front()) / med};
}

inline void row(const char* group, const char* workload, const char* impl, Result r, double bytes = 0) {
	if (bytes > 0)
		std::printf("%-12s %-28s %-22s %10.1f ns  spread %5.1f%%  %7.2f GB/s\n", group, workload, impl, r.med_ns, r.spread * 100, bytes / r.med_ns);
	else
		std::printf("%-12s %-28s %-22s %10.1f ns  spread %5.1f%%\n", group, workload, impl, r.med_ns, r.spread * 100);
	std::fflush(stdout);
}
}  // namespace bh
