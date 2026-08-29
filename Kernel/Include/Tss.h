#pragma once

#include <Types.h>

extern UINT64 SyscallStackTop;
extern UINT64 SyscallUserRsp;

VOID TssInit(VOID);
VOID TssSetRsp0(UINT64 Rsp0);