/*
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
#include <cstring>                                // for std::strncmp

#include "chars.hh"                               // for chars::hexdec


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


std::string
urldecode(const void *p, size_t size, char plus, char amp, char colon, char eq, char encoded)
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
				auto dec = chars::hexdec(&q);
				if (dec >= 0) {
					// Reset c, try the special characters again
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
	query = urldecode(q, ' ', '\0', '\0', '\1', '%');
	query_decoded = urldecode(q);
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
	path = urldecode(np, ' ', '&', ';', '=', '%');
	path_decoded = urldecode(np);

	char cn;
	size_t length;
	const char *ni = path.data();
	const char *nf = ni + path.size();
	const char *n0, *n1 , *ns= nullptr;
	size_t takeoff = 0, addin=0, slc_level = 0;
	State state;

	state = State::CMD;

	off = ni;

	if (path.back() == '/') {
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
