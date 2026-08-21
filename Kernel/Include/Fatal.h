#pragma once

#include <Types.h>
#include <Interrupt.h>

VOID FatalImpl(const CHAR *Message, const CHAR *File, INT Line, const CHAR *Func, UINTPTR CallerRip);
VOID FatalWithFrame(const CHAR *Message, InterruptFrame *Frame, const CHAR *File, INT Line, const CHAR *Func);

#define Fatal(Message) FatalImpl(Message, __FILE__, __LINE__, __func__, (UINTPTR)__builtin_return_address(0))
#define FatalF(Message, Frame) FatalWithFrame(Message, Frame, __FILE__, __LINE__, __func__)