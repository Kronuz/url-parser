// The declarations of the original library (4424e25) under other names, so one binary holds it
// beside the library under test. The sweep fetches it into <build>/sweep-src/orig.
#pragma once
#define urldecode orig_urldecode_lib
#define QueryParser OrigQueryParser
#define PathParser OrigPathParser
#define command__ orig_command__
#include "orig/url_parser.h"
#undef urldecode
#undef QueryParser
#undef PathParser
#undef command__
