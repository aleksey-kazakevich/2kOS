#pragma once

#include <Types.h>
#include <Lib/StdArg.h>

INT VsnPrintf(CHAR *Str, USIZE Size, const CHAR *Fmt, VA_LIST Args);
INT SnPrintf(CHAR *Str, USIZE size, const CHAR *Fmt, ...);
INT SPrintf(CHAR *Str, const CHAR *Fmt, ...);