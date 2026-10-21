#pragma once

#include <Types.h>

typedef struct {
    UINT64 Tick;
    INT    Pid;
    CHAR   Name[32];
    UINT64 Num;
    UINT64 Args[6];
    UINT64 Ret;
    BOOL   Valid;
} SyscallLogEntry;

#define SYSCALL_LOG_SIZE 128

VOID SyscallLogInit(VOID);
INT  SyscallLogPush(UINT64 Num, UINT64 *Args, UINT64 Ret);
VOID SyscallLogClear(VOID);
UINT32 SyscallLogCount(VOID);
const SyscallLogEntry* SyscallLogGet(UINT32 Index);
VOID SyscallLogDump(VOID);
VOID SyscallLogSetEnabled(BOOL Enabled);
BOOL SyscallLogIsEnabled(VOID);