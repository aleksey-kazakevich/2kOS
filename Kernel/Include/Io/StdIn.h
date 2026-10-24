#pragma once

#include <Io/StdIo.h>

#define StdInRead(b,n,r)    StdIoReadChannel(STDIO_CHAN_STDIN, (b), (n), (r))