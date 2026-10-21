#include <Utils/SyscallLog.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Drivers/Hpet.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Syscall.h>
#include <Basecon.h>
#include <Return.h>

STATIC SyscallFunction GSyscallTable[SYSCALL_MAX] = {0};
STATIC UINT32 GSyscallCount = 0;

extern Timer GTimer;

STATIC BOOL IsUserPointer(VOID *Ptr) {
    UINT64 Addr = (UINT64)Ptr;
    // User addresses in the range: 0x400000 - 0x7FFFFFFFF000
    return (Addr >= 0x400000 && Addr < 0x00007FFFFFFFFFFFULL);
}

STATIC INT CopyFromUser(VOID *Dest, const VOID *Src, USIZE Size) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return -1;

    UINT8 *D = (UINT8*)Dest;
    UINT64 SrcAddr = (UINT64)Src;

    while (Size > 0) {
        UINT64 Phys = PagingLookupVirt(Current->PageTable, SrcAddr);
        if (!Phys) return -1;  // page not mapped

        UINT64 Off = SrcAddr & (PAGE_SIZE - 1);
        USIZE Chunk = PAGE_SIZE - Off;
        if (Chunk > Size) Chunk = Size;

        MemCpy(D, (VOID*)PhysToVirt(Phys), Chunk);

        D += Chunk;
        SrcAddr += Chunk;
        Size -= Chunk;
    }
    return 0;
}

STATIC INT CopyToUser(VOID *Dest, const VOID *Src, USIZE Size) {
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

STATIC CHAR* CopyStringFromUser(const CHAR *UserStr) {
    if (!IsUserPointer((VOID*)UserStr)) return NULLPTR;
    
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return NULLPTR;
    
    // Count length
    USIZE Len = 0;
    while (Len < 4096) {  // Max length of string
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

STATIC UINT64 SysWriteHandler(UINT64 Fd, UINT64 Buf, UINT64 Count) {
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

STATIC UINT64 SysExitHandler(UINT64 Code) {
    TaskExit((INT)Code);
    return 0;
}

STATIC UINT64 SysGetPidHandler(VOID) {
    Task *Current = GetCurrentTask();
    if (!Current) return 0;
    return Current->Pid;
}

STATIC UINT64 TimerGetMicroseconds(VOID) {
    if (HpetIsAvailable()) {
        UINT64 Freq = HpetGetFrequency();
        UINT64 Counter = HpetReadCounter();
        if (Freq == 0) return 0;
        return (Counter * 1000000ULL) / Freq;
    }
    // APIC: 1000 Hz, tick = 1 ms = 1000 us
    return GTimer.Ticks * 1000ULL;
}

STATIC UINT64 SysGetTicksHandler(VOID) {
    return TimerGetMicroseconds();
}

STATIC UINT64 SysBrkHandler(UINT64 NewBrk) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->IsUser) {
        return (UINT64)-1;
    }

    UINT64 OldBrk = Current->BrkCurrent;

    // brk(0) — return the current boundary, change nothing
    if (NewBrk == 0) {
        return OldBrk;
    }

    // Bounds
    if (NewBrk < Current->BrkStart) {
        return OldBrk;   // cannot go below the start of the heap
    }
    if (NewBrk > Current->BrkMax) {
        return OldBrk;   // cannot go above the limit
    }

    // Align to pages
    UINT64 OldPage = (OldBrk  + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    UINT64 NewPage = (NewBrk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (NewPage > OldPage) {
        UINT64 Flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NO_EXEC;

        // Remember the first page we mapped,
        // so we can roll everything back on OOM.
        UINT64 FirstMapped = 0;
        BOOL AnyMapped = FALSE;

        for (UINT64 Va = OldPage; Va < NewPage; Va += PAGE_SIZE) {
            // Already mapped — skip (e.g. an adjacent segment)
            if (PagingLookupVirt(Current->PageTable, Va) != 0) {
                continue;
            }

            VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
            if (!Phys) {
                if (AnyMapped) {
                    for (UINT64 Rollback = FirstMapped;
                         Rollback < Va;
                         Rollback += PAGE_SIZE) {
                        UINT64 RollbackPhys =
                            PagingLookupVirt(Current->PageTable, Rollback);
                        if (RollbackPhys) {
                            PagingUnmapPage(Current->PageTable, Rollback);
                            PhysAllocFreePage(PhysAllocGet(),
                                (VOID *)(RollbackPhys & ~(PAGE_SIZE - 1)));
                        }
                    }
                }
                return OldBrk;
            }

            // Zero the physical page
            MemSet((VOID *)PhysToVirt((UINT64)Phys), 0, PAGE_SIZE);

            // Map into user-space
            INT Rc = PagingMapPage(Current->PageTable, Va,
                                   (UINT64)Phys, Flags);
            if (Rc != SUCCESS) {
                PhysAllocFreePage(PhysAllocGet(), Phys);

                if (AnyMapped) {
                    for (UINT64 Rollback = FirstMapped;
                         Rollback < Va;
                         Rollback += PAGE_SIZE) {
                        UINT64 RollbackPhys =
                            PagingLookupVirt(Current->PageTable, Rollback);
                        if (RollbackPhys) {
                            PagingUnmapPage(Current->PageTable, Rollback);
                            PhysAllocFreePage(PhysAllocGet(),
                                (VOID *)(RollbackPhys & ~(PAGE_SIZE - 1)));
                        }
                    }
                }
                return OldBrk;
            }

            if (!AnyMapped) {
                FirstMapped = Va;
                AnyMapped = TRUE;
            }
        }
    } else if (NewPage < OldPage) {
        for (UINT64 Va = NewPage; Va < OldPage; Va += PAGE_SIZE) {
            UINT64 Phys = PagingLookupVirt(Current->PageTable, Va);
            if (Phys) {
                PagingUnmapPage(Current->PageTable, Va);
                PhysAllocFreePage(PhysAllocGet(),
                                  (VOID *)(Phys & ~(PAGE_SIZE - 1)));
            }
        }
    }

    Current->BrkCurrent = NewBrk;
    return NewBrk;
}

INT SyscallRegister(UINT64 Num, SyscallFunction Func) {
    if (Num >= SYSCALL_MAX) return INCORRECT_VALUE;
    if (GSyscallTable[Num] != NULLPTR) return ALREADY_EXISTS;
    
    GSyscallTable[Num] = Func;
    GSyscallCount++;
    return SUCCESS;
}

STATIC VOID SyscallRegisterAll(VOID) {
    SyscallRegister(SYS_WRITE, (SyscallFunction)SysWriteHandler);
    SyscallRegister(SYS_BRK, (SyscallFunction)SysBrkHandler);
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
    UINT64 Args[6] = { Arg1, Arg2, Arg3, Arg4, Arg5, Arg6 };
    UINT64 Ret;

    // Check syscall number
    if (Num >= SYSCALL_MAX) {
        SyscallLogPush(Num, Args, (UINT64)-1);
        return -1;
    }
    
    SyscallFunction Func = GSyscallTable[Num];
    if (!Func) {
        SyscallLogPush(Num, Args, (UINT64)-1);
        return -1;
    }
    
    // Call handler
    Ret = Func(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6);
    SyscallLogPush(Num, Args, Ret);
    return Ret;
}