#pragma once

// Using built-in VA_LIST
typedef __builtin_va_list VA_LIST;

#define VaStart(Ap, Last) __builtin_va_start(Ap, Last)
#define VaArg(Ap, Type)   __builtin_va_arg(Ap, Type)
#define VaEnd(Ap)         __builtin_va_end(Ap)
#define VaCopy(Dest, Src) __builtin_va_copy(Dest, Src)
