// Kernel/Include/Syscall.h
#pragma once

#include <Types.h>

// ============================================================================
// System call numbers
// ============================================================================

#define SYS_WRITE          1
#define SYS_EXIT           60
#define SYS_GETPID         39
#define SYS_GETTICKS       1001

#define SYSCALL_MAX        2048

typedef UINT64 (*SyscallFunction)(UINT64 Arg1, UINT64 Arg2, UINT64 Arg3,
                                  UINT64 Arg4, UINT64 Arg5, UINT64 Arg6);

static inline UINT64 Syscall(UINT64 Num, UINT64 Arg1, UINT64 Arg2, 
                              UINT64 Arg3, UINT64 Arg4, UINT64 Arg5) {
    UINT64 Result;
    
    __asm__ volatile (
        "mov %1, %%rax\n\t"
        "mov %2, %%rdi\n\t"
        "mov %3, %%rsi\n\t"
        "mov %4, %%rdx\n\t"
        "mov %5, %%r10\n\t"
        "mov %6, %%r8\n\t"
        "syscall\n\t"
        "mov %%rax, %0"
        : "=r"(Result)
        : "r"(Num), "r"(Arg1), "r"(Arg2), "r"(Arg3), "r"(Arg4), "r"(Arg5)
        : "rax", "rcx", "r11", "rdi", "rsi", "rdx", "r10", "r8", "memory"
    );
    
    return Result;
}

static inline UINT64 SysWrite(UINT64 Fd, const CHAR *Buf, UINT64 Count) {
    return Syscall(SYS_WRITE, Fd, (UINT64)Buf, Count, 0, 0);
}

static inline UINT64 SysExit(UINT64 Code) {
    return Syscall(SYS_EXIT, Code, 0, 0, 0, 0);
}

static inline UINT64 SysGetPid(VOID) {
    return Syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline UINT64 SysGetTicks(VOID) {
    return Syscall(SYS_GETTICKS, 0, 0, 0, 0, 0);
}

VOID SyscallInit(VOID);
UINT64 SyscallHandler(UINT64 Num, UINT64 Arg1, UINT64 Arg2, UINT64 Arg3,
                       UINT64 Arg4, UINT64 Arg5, UINT64 Arg6);
INT SyscallRegister(UINT64 Num, SyscallFunction Func);
