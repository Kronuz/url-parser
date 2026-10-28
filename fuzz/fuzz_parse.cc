// libFuzzer target: urldecode, QueryParser and PathParser over arbitrary bytes.
// Build: cmake -B build -DURL_PARSER_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "url_parser.h"


extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	// An exactly-sized block, so ASan sees any read past the input.
	auto buf = std::make_unique<char[]>(size ? size : 1);
	if (size) {
		std::memcpy(buf.get(), data, size);
	}
	std::string_view in(buf.get(), size);

	(void)urldecode(in);
	(void)urldecode(buf.get(), size, '+', '|', ',', ':', '%');

	QueryParser qp;
	qp.init(in);
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
