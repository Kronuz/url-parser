// Frozen copy of the original byte-at-a-time urldecode, kept only as the oracle
// for test/urldecode_diff.cc. Do not "improve" it: its job is to stay identical
// to the behavior the library had before the single-pass rewrite.
#pragma once

#include <string>

#include "chars.hh"

inline std::string
old_urldecode(const void *p, size_t size, char plus = ' ', char amp = '&', char colon = ';', char eq = '=', char encoded = '\0')
{
	std::string buf;
	buf.reserve(size);
	const auto* q = (const char *)p;
	auto p_end = q + size;
	while (q != p_end) {
		char c = *q++;
		switch (c) {
			case '+':
				buf.push_back(plus);
				break;
			case '&':
				buf.push_back(amp);
				break;
			case ';':
				buf.push_back(colon);
				break;
			case '=':
				buf.push_back(eq);
				break;
			case '%': {
				auto dec = (p_end - q >= 2) ? chars::hexdec(&q) : -1;
				if (dec >= 0) {
					c = (encoded && dec != '{' && dec != ',' && dec != '}')
						? encoded
						: dec;
				}
			}
			[[fallthrough]];
			default:
				switch (c) {
					case '+':
						buf.push_back(plus);
						break;
					case '&':
						buf.push_back(amp);
						break;
					case ';':
						buf.push_back(colon);
						break;
					case '=':
						buf.push_back(eq);
						break;
					default:
						buf.push_back(c);
				}
		}
	}
	return buf;
}
