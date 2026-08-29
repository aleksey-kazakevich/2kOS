#pragma once

#include <Types.h>

// The frame that the processor saves on the stack when an exception occurs
typedef struct {
    // Stored registers (in PUSH order in stub assembly)
    UINT64 R15;
    UINT64 R14;
    UINT64 R13;
    UINT64 R12;
    UINT64 R11;
    UINT64 R10;
    UINT64 R9;
    UINT64 R8;
    UINT64 RDI;
    UINT64 RSI;
    UINT64 RBP;
    UINT64 RBX;
    UINT64 RDX;
    UINT64 RCX;
    UINT64 RAX;
    UINT64 ErrorCode;
    UINT64 RIP;
    UINT64 CS;
    UINT64 RFLAGS;
    UINT64 RSP;
    UINT64 SS;
} InterruptFrame;

// For exceptions without error code (Divide Error, Debug, NMI, Breakpoint, Overflow, Bound, Invalid Opcode, Device Not Available)
// ErrorCode will be 0xFFFFFFFF ("no error code" marker)