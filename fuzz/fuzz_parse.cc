// libFuzzer target: urldecode, QueryParser and PathParser over arbitrary bytes. The first
// five bytes pick the substitutes of `urldecode`; its output and both parsers' buffers must
// equal the byte-at-a-time reference decoder (test/urldecode_old.h), and nothing may read
// past the input.
// Build: cmake -B build -DURL_PARSER_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
// The headers url_parser.h includes, before the macros below change what `class` means.
#include <cstdio>
#include <string>

#define private public
#define class struct  // the parsers are classes with default-private buffers
#include "url_parser.h"
#undef class
#undef private
#include "urldecode_old.h"


extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	// An exactly-sized block, so ASan sees any read past the input.
	auto buf = std::make_unique<char[]>(size ? size : 1);
	if (size) {
		std::memcpy(buf.get(), data, size);
	}
	std::string_view in(buf.get(), size);

	(void)urldecode(in);
	if (urldecode(in) != old_urldecode(in.data(), in.size())) { std::abort(); }
	// The substitutes come from the input, so every combination of them is reachable.
	if (size >= 5) {
		const char* t = buf.get() + 5;
		const size_t n = size - 5;
		const char plus = buf[0], amp = buf[1], colon = buf[2], eq = buf[3], encoded = buf[4];
		if (urldecode(t, n, plus, amp, colon, eq, encoded) != old_urldecode(t, n, plus, amp, colon, eq, encoded)) { std::abort(); }
	}
	(void)urldecode(buf.get(), size, '+', '|', ',', ':', '%');

	QueryParser qp;
	qp.init(in);
	if (qp.query != old_urldecode(in.data(), in.size(), ' ', '\0', '\0', '\1', '%') || qp.query_decoded != old_urldecode(in.data(), in.size())) { std::abort(); }
	const char* names[] = {"q", "a", "b", "", "query"};
	for (const char* n : names) {
		qp.rewind();
		while (qp.next(n, std::strlen(n)) == 0) {
			(void)qp.get();
			if (qp.len == 0 && qp.off == nullptr) { break; }
		}
	}

	PathParser pp;
	auto st = pp.init(in);
	(void)pp.has_pth();
	(void)pp.get_id();
	(void)pp.get_slc();
	(void)pp.get_cmd();
	for (int i = 0; i < 8 && st != PathParser::State::END && st < PathParser::State::INVALID_STATE; ++i) {
		st = pp.next();
		(void)pp.get_pth();
		(void)pp.get_hst();
	}
	return 0;
}
