// urldecode / QueryParser::init vs the frozen original, a single-pass LUT decoder and
// (when built with ada) ada::unicode::percent_decode. Time it under a quiet hold.
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "harness.h"
#include "url_parser.h"
#include "urldecode_old.h"
#ifdef HAVE_ADA
#include "ada.h"
#endif

static std::string lut_decode(std::string_view s) {
	static const struct Lut {
		int8_t v[256];
		Lut() { for (int i = 0; i < 256; ++i) v[i] = -1;
			for (int i = 0; i < 10; ++i) v['0' + i] = i;
			for (int i = 0; i < 6; ++i) { v['a' + i] = 10 + i; v['A' + i] = 10 + i; } }
	} lut;
	std::string out(s.size(), '\0');
	char* d = out.data();
	const char* p = s.data();
	const char* e = p + s.size();
	while (p < e) {
		char c = *p++;
		if (c == '+') { *d++ = ' '; continue; }
		if (c == '%' && e - p >= 2) {
			int a = lut.v[(uint8_t)p[0]], b = lut.v[(uint8_t)p[1]];
			if ((a | b) >= 0) { char dec = char(a << 4 | b); p += 2; *d++ = dec == '+' ? ' ' : dec; continue; }
		}
		*d++ = c;
	}
	out.resize(d - out.data());
	return out;
}

// kind: 0 clean (no '%' or '+'), 1 mostly clean (a few escapes and '+'), 2 25% "%XX".
static std::string make_query(size_t target, int kind, std::mt19937& rng) {
	std::string s;
	int k = 0;
	while (s.size() < target) {
		if (k) s += '&';
		s += "key" + std::to_string(k++) + "=";
		int len = 3 + rng() % 12;
		for (int i = 0; i < len; ++i) {
			unsigned r = rng() % 100;
			if (kind == 2 && r < 25) { static const char* h = "0123456789ABCDEF"; s += '%'; s += h[rng() % 16]; s += h[rng() % 16]; }
			else if (kind == 1 && r < 3) s += "%20";
			else if (kind == 1 && r < 8) s += '+';
			else s += char('a' + rng() % 26);
		}
	}
	s.resize(target);
	return s;
}

int main() {
	std::mt19937 rng(42);
	const char* names[] = {"clean", "few-escapes", "25%-escapes"};
	for (size_t sz : {32u, 256u, 4096u, 65536u}) {
		for (int kind = 0; kind < 3; ++kind) {
			std::string q = make_query(sz, kind, rng);
			std::string wl = "urldecode " + std::to_string(sz) + "B " + names[kind];
			if (urldecode(q) != old_urldecode(q.data(), q.size()) || lut_decode(q) != urldecode(q)) { std::printf("MISMATCH %s\n", wl.c_str()); return 1; }
			auto cur = [&](uint64_t n) { for (uint64_t i = 0; i < n; ++i) { auto s = urldecode(q); bh::use(s); } };
			auto old = [&](uint64_t n) { for (uint64_t i = 0; i < n; ++i) { auto s = old_urldecode(q.data(), q.size()); bh::use(s); } };
			auto lut = [&](uint64_t n) { for (uint64_t i = 0; i < n; ++i) { auto s = lut_decode(q); bh::use(s); } };
			bh::row("url", wl.c_str(), "current #1", bh::measure(cur), q.size());
			bh::row("url", wl.c_str(), "current #2 (A/A)", bh::measure(cur), q.size());
			bh::row("url", wl.c_str(), "frozen old", bh::measure(old), q.size());
			bh::row("url", wl.c_str(), "LUT single-pass", bh::measure(lut), q.size());
#ifdef HAVE_ADA
			// ada has no '+' handling and no %2B special case: compare on '+'-free input.
			std::string np = q;
			for (auto& c : np) if (c == '+') c = 'x';
			for (size_t k = 0; k + 2 < np.size(); ++k) if (np[k] == '%' && np[k + 1] == '2' && (np[k + 2] == 'B' || np[k + 2] == 'b')) np[k + 2] = 'C';
			auto ada_dec = [&](const std::string& in) {
				size_t f = in.find('%');
				return f == std::string::npos ? in : ada::unicode::percent_decode(in, f);
			};
			if (ada_dec(np) != urldecode(np)) { std::printf("MISMATCH ada %s\n", wl.c_str()); return 1; }
			auto ad = [&](uint64_t n) { for (uint64_t i = 0; i < n; ++i) { auto s = ada_dec(np); bh::use(s); } };
			auto ours_np = [&](uint64_t n) { for (uint64_t i = 0; i < n; ++i) { auto s = urldecode(np); bh::use(s); } };
			bh::row("url", wl.c_str(), "current ('+'-free)", bh::measure(ours_np), q.size());
			bh::row("url", wl.c_str(), "ada percent_decode", bh::measure(ad), q.size());
#endif
		}
	}

	// QueryParser: init + 3 key lookups (typical request).
	for (size_t sz : {64u, 512u, 4096u}) {
		std::string q = make_query(sz, 1, rng);
		std::string wl = "QueryParser init+3next " + std::to_string(sz) + "B";
		auto run = [&](uint64_t n) {
			QueryParser qp;
			for (uint64_t i = 0; i < n; ++i) {
				qp.init(q);
				qp.rewind(); if (qp.next("key0") == 0) bh::use(qp.get());
				qp.rewind(); if (qp.next("key1") == 0) bh::use(qp.get());
				qp.rewind(); if (qp.next("nokey") == 0) bh::use(qp.get());
			}
		};
		bh::row("url", wl.c_str(), "current #1", bh::measure(run), q.size());
		bh::row("url", wl.c_str(), "current #2 (A/A)", bh::measure(run), q.size());
		auto old = [&](uint64_t n) {
			for (uint64_t i = 0; i < n; ++i) {
				auto a = old_urldecode(q.data(), q.size(), ' ', '\0', '\0', '\1', '%');
				auto b = old_urldecode(q.data(), q.size());
				bh::use(a); bh::use(b);
			}
		};
		bh::row("url", wl.c_str(), "old init decode x2", bh::measure(old), q.size());
	}
}
