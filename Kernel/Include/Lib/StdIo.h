#pragma once

#include <Lib/StdArg.h>
#include <Types.h>

INT VsnPrintf(CHAR *Str, USIZE Size, const CHAR *Fmt, VA_LIST Args);
INT SnPrintf(CHAR *Str, USIZE size, const CHAR *Fmt, ...);
INT SPrintf(CHAR *Str, const CHAR *Fmt, ...);