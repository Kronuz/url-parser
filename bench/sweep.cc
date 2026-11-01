// Same-run A/B sweep of the public entry points. Variants (each a whole url_parser.cc under other names):
//   new   the library under test (the working tree), timed twice (the A/A column)
//   orig  4424e25: the original parsers, and test/urldecode_old.h (the frozen reference) for urldecode
//   prev  9ce4c23: the decoder before the small-input work
//   base  any other commit, given to CMake as -DURL_PARSER_SWEEP_BASE=<commit>
// The variants are fetched when the sweep is configured (URL_PARSER_SWEEP=ON) and built into one binary
// under other names. They are interleaved round by round and rotated, so drift hits all of them alike.
// Usage: url_parser_sweep [filter] [rounds] [mixed] [nmax] [parts] [variants]
//   filter    only the rows whose name contains it (default: all)
//   rounds    interleaved rounds per row (default 15)
//   mixed     1: cycle each row through 8 inputs instead of one
//   nmax      the largest input size (default 4096)
//   parts     1: also the rows of QueryParser::init and next alone
//   variants  letters of the variants to run beside new: o p b (default op)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "orig_api.h"
#include "prev_api.h"
#ifdef URL_PARSER_SWEEP_BASE_SET
#include "base_api.h"
#endif
#include "url_parser.h"
#include "urldecode_old.h"

using clk = std::chrono::steady_clock;
template <typename T> static inline void use(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }

static std::string make(size_t n, int kind, std::mt19937& rng) {
	std::string s;
	while (s.size() < n) {
		unsigned r = rng() % 100;
		if (kind == 2 && r < 25) { static const char* h = "0123456789ABCDEF"; s += '%'; s += h[rng() % 16]; s += h[rng() % 16]; }
		else if (kind == 1 && r < 4) s += "%20";
		else if (kind == 1 && r < 9) s += '+';
		else if (kind == 3 && r < 15) s += "&=;"[rng() % 3];
		else if (kind == 4) {  // a realistic query: k=v&k=v with an escape or a plus now and then
			s += "k" + std::to_string(rng() % 10) + "=";
			int len = 2 + rng() % 8;
			for (int i = 0; i < len; ++i) {
				unsigned q = rng() % 100;
				if (q < 6) s += "%2F"; else if (q < 10) s += '+'; else s += char('a' + rng() % 26);
			}
			s += '&';
		}
		else s += char('a' + rng() % 26);
	}
	s.resize(n);
	return s;
}

static std::string make_path(size_t n, std::mt19937& rng) {
	std::string s = "/idx";
	while (s.size() < n) { s += "/seg" + std::to_string(rng() % 100); }
	s.resize(n);
	return s;
}

using Inputs = std::shared_ptr<std::vector<std::string>>;
using Fn = std::function<void(uint64_t)>;

// One library: its decoder with the default and the query substitution sets, its two parsers.
#define QSARGS ' ', '\0', '\0', '\1', '%'
#define LIB(NAME, DEF, QS, QPT, PPT)                                                                                           \
	struct NAME {                                                                                                              \
		static Fn def(Inputs in, size_t mask) {                                                                                \
			return [in, mask](uint64_t it) { for (uint64_t i = 0; i < it; ++i) { const auto& q = (*in)[i & mask]; auto s = DEF; use(s); } }; \
		}                                                                                                                      \
		static Fn qs(Inputs in, size_t mask) {                                                                                 \
			return [in, mask](uint64_t it) { for (uint64_t i = 0; i < it; ++i) { const auto& q = (*in)[i & mask]; auto s = QS; use(s); } }; \
		}                                                                                                                      \
		using QP = QPT;                                                                                                        \
		using PP = PPT;                                                                                                        \
	};

LIB(LNew, urldecode(q.data(), q.size()), urldecode(q.data(), q.size(), QSARGS), QueryParser, PathParser)
LIB(LOrig, old_urldecode(q.data(), q.size()), old_urldecode(q.data(), q.size(), QSARGS), OrigQueryParser, OrigPathParser)
LIB(LPrev, prev_urldecode_lib(q.data(), q.size()), prev_urldecode_lib(q.data(), q.size(), QSARGS), PrevQueryParser, PrevPathParser)
#ifdef URL_PARSER_SWEEP_BASE_SET
LIB(LBase, base_urldecode_lib(q.data(), q.size()), base_urldecode_lib(q.data(), q.size(), QSARGS), BaseQueryParser, BasePathParser)
#endif

template <typename L> static Fn qp_run(Inputs in, size_t mask) {
	return [in, mask](uint64_t it) {
		typename L::QP p;
		for (uint64_t i = 0; i < it; ++i) {
			p.init((*in)[i & mask]);
			p.rewind(); if (p.next("a") == 0) use(p.get());
			p.rewind(); if (p.next("b") == 0) use(p.get());
			p.rewind(); if (p.next("zz") == 0) use(p.get());
		}
	};
}
template <typename L> static Fn qpi_run(Inputs in, size_t mask) {
	return [in, mask](uint64_t it) {
		typename L::QP p;
		for (uint64_t i = 0; i < it; ++i) { p.init((*in)[i & mask]); use(p); }
	};
}
template <typename L> static Fn qpn_run(Inputs in, size_t mask) {
	return [in, mask](uint64_t it) {
		typename L::QP p;
		p.init((*in)[0]);
		for (uint64_t i = 0; i < it; ++i) {
			p.rewind(); if (p.next("a") == 0) use(p.get());
			p.rewind(); if (p.next("b") == 0) use(p.get());
			p.rewind(); if (p.next("zz") == 0) use(p.get());
		}
	};
}
template <typename L> static Fn pp_run(Inputs in, size_t mask) {
	return [in, mask](uint64_t it) {
		using PPt = typename L::PP;
		PPt q;
		for (uint64_t i = 0; i < it; ++i) {
			auto st = q.init((*in)[i & mask]);
			int k = 0;
			while (st != PPt::State::END && st < PPt::State::INVALID_STATE && k++ < 8) { st = q.next(); use(q.get_pth()); }
		}
	};
}

struct Var { const char* name; char letter; };
static const Var vars[] = {{"new", 'n'}, {"orig", 'o'}, {"prev", 'p'}, {"base", 'b'}};
static constexpr int NV = 4;

struct Row {
	std::string name;
	Fn f[NV];
};

static double time_one(const Fn& f, uint64_t n) {
	auto t0 = clk::now();
	f(n);
	return std::chrono::duration<double, std::nano>(clk::now() - t0).count() / double(n);
}
static double median(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; }

int main(int argc, char** argv) {
	const char* filter = argc > 1 ? argv[1] : "";
	const int rounds = argc > 2 ? atoi(argv[2]) : 15;
	const bool mixed = argc > 3 && atoi(argv[3]) != 0;
	const size_t nmax = argc > 4 ? (size_t)atoi(argv[4]) : 4096;
	const bool parts = argc > 5 && atoi(argv[5]) != 0;
	const char* want = argc > 6 ? argv[6] : "op";  // letters of the variants to run (new is always run)
	const double target_ms = 8;
	std::mt19937 rng(7);
	const char* kinds[] = {"clean", "few-esc", "25%esc", "seps", "query"};
	std::vector<Row> rows;
	const size_t K = mixed ? 8 : 1;
	const size_t mask = K - 1;

	auto add = [&](const std::string& name, auto make_fn) {
		Row r;
		r.name = name;
		r.f[0] = make_fn(LNew{});
		r.f[1] = make_fn(LOrig{});
		r.f[2] = make_fn(LPrev{});
#ifdef URL_PARSER_SWEEP_BASE_SET
		r.f[3] = make_fn(LBase{});
#endif
		rows.push_back(std::move(r));
	};

	for (size_t n : {4u, 8u, 12u, 15u, 16u, 17u, 24u, 31u, 32u, 33u, 48u, 64u, 128u, 256u, 1024u, 4096u}) {
		if (n > nmax) continue;
		for (int kind = 0; kind < 5; ++kind) {
			auto in = std::make_shared<std::vector<std::string>>();
			for (size_t k = 0; k < K; ++k) in->push_back(make(n, kind, rng));
			char nm[96];
			std::snprintf(nm, sizeof nm, "urldecode(def)  n=%zu %s", n, kinds[kind]);
			add(nm, [&](auto L) { return decltype(L)::def(in, mask); });
			std::snprintf(nm, sizeof nm, "urldecode(qs)   n=%zu %s", n, kinds[kind]);
			add(nm, [&](auto L) { return decltype(L)::qs(in, mask); });
			std::snprintf(nm, sizeof nm, "QueryParser     n=%zu %s", n, kinds[kind]);
			add(nm, [&](auto L) { return qp_run<decltype(L)>(in, mask); });
			if (parts) {
				std::snprintf(nm, sizeof nm, "QP init only     n=%zu %s", n, kinds[kind]);
				add(nm, [&](auto L) { return qpi_run<decltype(L)>(in, mask); });
				std::snprintf(nm, sizeof nm, "QP next only     n=%zu %s", n, kinds[kind]);
				add(nm, [&](auto L) { return qpn_run<decltype(L)>(in, mask); });
			}
		}
	}
	for (size_t n : {8u, 12u, 16u, 24u, 32u, 48u, 64u, 256u}) {
		if (n > nmax) continue;
		auto in = std::make_shared<std::vector<std::string>>();
		for (size_t k = 0; k < K; ++k) in->push_back(make_path(n, rng));
		char nm[96];
		std::snprintf(nm, sizeof nm, "PathParser      n=%zu", n);
		add(nm, [&](auto L) { return pp_run<decltype(L)>(in, mask); });
	}

	bool on[NV];
	for (int v = 0; v < NV; ++v) on[v] = v == 0 || std::strchr(want, vars[v].letter);
#ifndef URL_PARSER_SWEEP_BASE_SET
	on[3] = false;  // no base commit was configured
#endif
	std::printf("%-30s %8s", "row", "new");
	for (int v = 1; v < NV; ++v) if (on[v]) std::printf(" %8s", vars[v].name);
	for (int v = 1; v < NV; ++v) if (on[v]) std::printf(" %6s/n", vars[v].name);
	std::printf("  A/A\n");

	int shown = 0, under = 0;
	double worst = 1e9;
	for (auto& r : rows) {
		if (*filter && r.name.find(filter) == std::string::npos) continue;
		uint64_t n = 16;
		for (;;) {
			auto t0 = clk::now();
			r.f[0](n);
			double ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
			if (ms >= 2) { n = std::max<uint64_t>(16, uint64_t(double(n) * target_ms / ms)); break; }
			n *= 4;
		}
		n = (n + 7) & ~uint64_t(7);
		std::vector<std::vector<double>> t(NV + 1);  // the last one is the A/A
		std::vector<int> act;
		for (int v = 0; v < NV; ++v) if (on[v]) act.push_back(v);
		act.push_back(NV);
		for (int v : act) r.f[v == NV ? 0 : v](n);  // warm
		const int na = (int)act.size();
		for (int k = 0; k < rounds; ++k)
			for (int j = 0; j < na; ++j) {
				int v = act[(k + j) % na];
				t[v].push_back(time_one(r.f[v == NV ? 0 : v], n));
			}
		std::vector<double> med(NV + 1, 0);
		for (int v : act) med[v] = median(t[v]);
		const double a = med[0];
		std::printf("%-30s %8.2f", r.name.c_str(), a);
		for (int v = 1; v < NV; ++v) if (on[v]) std::printf(" %8.2f", med[v]);
		double w = 1e9;
		for (int v = 1; v < NV; ++v) if (on[v]) { std::printf(" %8.3f", med[v] / a); if (v <= 2) w = std::min(w, med[v] / a); }
		std::printf(" %6.3f%s\n", med[NV] / a, w < 0.97 ? " <<<" : "");
		worst = std::min(worst, w);
		under += w < 0.97;
		++shown;
		std::fflush(stdout);
	}
	std::printf("rows %d, under 0.97 vs orig/prev: %d, worst %.3f\n", shown, under, worst);
}
