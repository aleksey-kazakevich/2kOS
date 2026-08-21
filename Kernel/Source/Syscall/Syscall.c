#include <Syscall.h>
#include <Basecon.h>
#include <Scheduler.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Return.h>
#include <Mem/Allocator.h>

static SyscallFunction GSyscallTable[SYSCALL_MAX] = {0};
static UINT32 GSyscallCount = 0;

static BOOL IsUserPointer(VOID *Ptr) {
    UINT64 Addr = (UINT64)Ptr;
    // User addresses in the range: 0x400000 - 0x7FFFFFFFF000
    return (Addr >= 0x400000 && Addr < 0x00007FFFFFFFFFFFULL);
}

static INT CopyFromUser(VOID *Dest, const VOID *Src, USIZE Size) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return -1;
    if (!IsUserPointer((VOID*)Src)) return -1;
    
    UINT8 *D = (UINT8*)Dest;
    const UINT8 *S = (const UINT8*)Src;
    
    for (USIZE I = 0; I < Size; I++) {
        UINT64 Phys = PagingLookupVirt(Current->PageTable, (UINT64)(S + I));
        if (!Phys) return -1;
        D[I] = *(UINT8*)PhysToVirt(Phys);
    }
    
    return 0;
}

static INT CopyToUser(VOID *Dest, const VOID *Src, USIZE Size) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return -1;
    if (!IsUserPointer(Dest)) return -1;
    
    const UINT8 *S = (const UINT8*)Src;
    UINT8 *D = (UINT8*)Dest;
    
    for (USIZE I = 0; I < Size; I++) {
        UINT64 Phys = PagingLookupVirt(Current->PageTable, (UINT64)(D + I));
        if (!Phys) return -1;
        *(UINT8*)PhysToVirt(Phys) = S[I];
    }
    
    return 0;
}

static CHAR* CopyStringFromUser(const CHAR *UserStr) {
    if (!IsUserPointer((VOID*)UserStr)) return NULLPTR;
    
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return NULLPTR;
    
    // Count lenght
    USIZE Len = 0;
    while (Len < 4096) {  // Max lenght of string
        UINT64 Phys = PagingLookupVirt(Current->PageTable, (UINT64)(UserStr + Len));
        if (!Phys) break;
        if (*(CHAR*)PhysToVirt(Phys) == '\0') break;
        Len++;
    }
    
    if (Len == 0 || Len >= 4096) return NULLPTR;
    
    CHAR *KernelStr = (CHAR*)MemoryAllocate(Len + 1);
    if (!KernelStr) return NULLPTR;
    
    if (CopyFromUser(KernelStr, UserStr, Len + 1) != 0) {
        MemoryFree(KernelStr);
        return NULLPTR;
    }
    
    return KernelStr;
}

static UINT64 SysWriteHandler(UINT64 Fd, UINT64 Buf, UINT64 Count) {
    if (Count == 0) return 0;
    if (!IsUserPointer((VOID*)Buf)) return -1;
    if (Count > 4096) Count = 4096;

    CHAR *KernelBuf = (CHAR*)MemoryAllocate(Count + 1);
    if (!KernelBuf) return -1;

    if (CopyFromUser(KernelBuf, (const VOID*)Buf, Count) != 0) {
        MemoryFree(KernelBuf);
        return -1;
    }
    KernelBuf[Count] = '\0';

    if (Fd == 1 || Fd == 2)
        BaseconPutString(KernelBuf);

    MemoryFree(KernelBuf);
    return Count;
}

static UINT64 SysExitHandler(UINT64 Code) {
    TaskExit((INT)Code);
    return 0;
}

static UINT64 SysGetPidHandler(VOID) {
    Task *Current = GetCurrentTask();
    if (!Current) return 0;
    return Current->Pid;
}

static UINT64 SysGetTicksHandler(VOID) {
    return TimerTicks();
}

INT SyscallRegister(UINT64 Num, SyscallFunction Func) {
    if (Num >= SYSCALL_MAX) return INCORRECT_VALUE;
    if (GSyscallTable[Num] != NULLPTR) return ALREADY_EXISTS;
    
    GSyscallTable[Num] = Func;
    GSyscallCount++;
    return SUCCESS;
}

static VOID SyscallRegisterAll(VOID) {
    SyscallRegister(SYS_WRITE, (SyscallFunction)SysWriteHandler);
    SyscallRegister(SYS_EXIT, (SyscallFunction)SysExitHandler);
    SyscallRegister(SYS_GETPID, (SyscallFunction)SysGetPidHandler);
    SyscallRegister(SYS_GETTICKS, (SyscallFunction)SysGetTicksHandler);
    
    BaseconPrintf(BASECON_TYPE_NORMAL, "syscall: %d handlers registered\n", GSyscallCount);
}

VOID SyscallInit(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "syscall: initializing...\n");
    
    // Clearing the table
    for (UINT64 I = 0; I < SYSCALL_MAX; I++) {
        GSyscallTable[I] = NULLPTR;
    }
    GSyscallCount = 0;
    
    // Register all syscalls
    SyscallRegisterAll();
    
    BaseconPrintf(BASECON_TYPE_SUCCESS, "syscall: initialization complete\n");
}

UINT64 SyscallHandler(UINT64 Num, UINT64 Arg1, UINT64 Arg2, UINT64 Arg3,
                       UINT64 Arg4, UINT64 Arg5, UINT64 Arg6) {
    // Check syscall number
    if (Num >= SYSCALL_MAX) {
        return -1;
    }
    
    SyscallFunction Func = GSyscallTable[Num];
    if (!Func) {
        return -1;
    }
    
    // Call handler
    return Func(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6);
}
