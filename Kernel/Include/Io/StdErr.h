#pragma once

#include <Io/StdIo.h>

#define StdErrPrint(s)      StdIoPrintChannel(STDIO_CHAN_STDERR, (s))
#define StdErrPrintf(...)   StdIoPrintfChannel(STDIO_CHAN_STDERR, __VA_ARGS__)
#define StdErrWrite(b,n)    StdIoWriteChannel(STDIO_CHAN_STDERR, (b), (n))