// The declarations of the previous library (9ce4c23, the decoder before the small-input work)
// under other names. The sweep fetches it into <build>/sweep-src/prev.
#pragma once
#define urldecode prev_urldecode_lib
#define QueryParser PrevQueryParser
#define PathParser PrevPathParser
#define command__ prev_command__
#include "prev/url_parser.h"
#undef urldecode
#undef QueryParser
#undef PathParser
#undef command__
