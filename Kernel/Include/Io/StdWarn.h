#pragma once

#include <Io/StdIo.h>

#define StdWarnPrint(s)     StdIoPrintChannel(STDIO_CHAN_STDWARN, (s))
#define StdWarnPrintf(...)  StdIoPrintfChannel(STDIO_CHAN_STDWARN, __VA_ARGS__)
#define StdWarnWrite(b,n)   StdIoWriteChannel(STDIO_CHAN_STDWARN, (b), (n))