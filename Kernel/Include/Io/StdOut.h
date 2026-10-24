#pragma once

#include <Io/StdIo.h>

#define StdOutPrint(s)      StdIoPrintChannel(STDIO_CHAN_STDOUT, (s))
#define StdOutPrintf(...)   StdIoPrintfChannel(STDIO_CHAN_STDOUT, __VA_ARGS__)
#define StdOutWrite(b,n)    StdIoWriteChannel(STDIO_CHAN_STDOUT, (b), (n))