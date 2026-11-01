// The declarations of the commit given as URL_PARSER_SWEEP_BASE under other names. The sweep
// fetches it into <build>/sweep-src/base.
#pragma once
#define urldecode base_urldecode_lib
#define QueryParser BaseQueryParser
#define PathParser BasePathParser
#define command__ base_command__
#include "base/url_parser.h"
#undef urldecode
#undef QueryParser
#undef PathParser
#undef command__
