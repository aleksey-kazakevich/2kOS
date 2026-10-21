#include <Utils/ExceptionLog.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <Types.h>

STATIC ExceptionLogEntry GLog[EXCEPTION_LOG_SIZE];
STATIC volatile UINT32 GLogHead = 0;
STATIC volatile UINT32 GLogCount = 0;
STATIC volatile UINT32 GLogTotal = 0;
STATIC SpinLock GLogLock;

VOID ExceptionLogInit(VOID) {
    SpinLockInit(&GLogLock);
    MemSet(GLog, 0, sizeof(GLog));
    GLogHead = 0;
    GLogCount = 0;
    GLogTotal = 0;
}

const CHAR* ExceptionTypeToString(ExceptionType Type) {
    switch (Type) {
        case EXC_DIVIDE_ERROR:         return "DIVIDE_ERROR";
        case EXC_DEBUG:                return "DEBUG";
        case EXC_NMI:                  return "NMI";
        case EXC_BREAKPOINT:           return "BREAKPOINT";
        case EXC_OVERFLOW:             return "OVERFLOW";
        case EXC_BOUND_RANGE:          return "BOUND_RANGE";
        case EXC_INVALID_OPCODE:       return "INVALID_OPCODE";
        case EXC_DEVICE_NOT_AVAILABLE: return "DEVICE_NOT_AVAILABLE";
        case EXC_DOUBLE_FAULT:         return "DOUBLE_FAULT";
        case EXC_INVALID_TSS:          return "INVALID_TSS";
        case EXC_SEGMENT_NOT_PRESENT:  return "SEGMENT_NOT_PRESENT";
        case EXC_STACK_SEGMENT_FAULT:  return "STACK_SEGMENT_FAULT";
        case EXC_GENERAL_PROTECTION:   return "GENERAL_PROTECTION";
        case EXC_PAGE_FAULT:           return "PAGE_FAULT";
        case EXC_FPU_ERROR:            return "FPU_ERROR";
        case EXC_ALIGNMENT_CHECK:      return "ALIGNMENT_CHECK";
        case EXC_MACHINE_CHECK:        return "MACHINE_CHECK";
        case EXC_SIMD_ERROR:           return "SIMD_ERROR";
        case EXC_VIRTUALIZATION:       return "VIRTUALIZATION";
        case EXC_CONTROL_PROTECTION:   return "CONTROL_PROTECTION";
        case EXC_HYPERVISOR_INJECTION: return "HYPERVISOR_INJECTION";
        case EXC_VMM_COMMUNICATION:    return "VMM_COMMUNICATION";
        case EXC_SECURITY:             return "SECURITY";
        case EXC_SYSCALL:              return "SYSCALL";
        default:                       return "UNKNOWN";
    }
}

INT ExceptionLogPush(ExceptionType Type, InterruptFrame *Frame) {
    if (!Frame) return -1;

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);

    UINT32 Idx = GLogHead;
    ExceptionLogEntry *E = &GLog[Idx];

    E->Tick = TimerTicks();
    E->Type = Type;
    E->Rip = Frame->RIP;
    E->ErrorCode = Frame->ErrorCode;
    E->Rsp = Frame->RSP;
    E->Cs = Frame->CS;
    E->Rflags = Frame->RFLAGS;

    if (Type == EXC_PAGE_FAULT) {
        E->FaultAddr = ReadCr2();
    } else {
        E->FaultAddr = 0;
    }

    Task *Current = GetCurrentTask();
    if (Current) {
        E->Pid = Current->Pid;
        StrnCpy(E->Name, Current->Name, sizeof(E->Name) - 1);
        E->Name[sizeof(E->Name) - 1] = '\0';
    } else {
        E->Pid = -1;
        StrCpy(E->Name, "<none>");
    }

    if ((Frame->CS & 0x3) != 0x3) {
        StrnCpy(E->Name, "kernel", sizeof(E->Name) - 1);
    }

    E->Valid = TRUE;

    GLogHead = (GLogHead + 1) % EXCEPTION_LOG_SIZE;
    if (GLogCount < EXCEPTION_LOG_SIZE) GLogCount++;
    GLogTotal++;

    SpinLockReleaseIrq(&GLogLock, Flags);
    return (INT)Idx;
}

VOID ExceptionLogClear(VOID) {
    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    MemSet(GLog, 0, sizeof(GLog));
    GLogHead = 0;
    GLogCount = 0;
    GLogTotal = 0;
    SpinLockReleaseIrq(&GLogLock, Flags);
}

UINT32 ExceptionLogCount(VOID) {
    return GLogCount;
}

const ExceptionLogEntry* ExceptionLogGet(UINT32 Index) {
    // Returns a pointer to an entry without the lock — the caller must
    // use it under the global lock or ensure serialization.
    // For safe use from dump — copy.
    if (Index >= GLogCount) return NULLPTR;

    UINT32 Start;
    if (GLogCount < EXCEPTION_LOG_SIZE) {
        Start = 0;
    } else {
        Start = GLogHead;
    }
    UINT32 Real = (Start + Index) % EXCEPTION_LOG_SIZE;
    if (!GLog[Real].Valid) return NULLPTR;
    return &GLog[Real];
}

VOID ExceptionLogDump(VOID) {
    // Snapshot under the lock
    ExceptionLogEntry Snapshot[EXCEPTION_LOG_SIZE];
    UINT32 Count;
    UINT32 Total;

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    Count = GLogCount;
    Total = GLogTotal;

    // Copy in chronological order
    UINT32 Start;
    if (Count < EXCEPTION_LOG_SIZE) Start = 0;
    else Start = GLogHead;

    for (UINT32 I = 0; I < Count; I++) {
        UINT32 Real = (Start + I) % EXCEPTION_LOG_SIZE;
        Snapshot[I] = GLog[Real];
    }

    SpinLockReleaseIrq(&GLogLock, Flags);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "\nException Log (%u entries, total %u)\n",
                  Count, Total);

    if (Count == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  (empty)\n\n");
        return;
    }

    for (UINT32 I = 0; I < Count; I++) {
        const ExceptionLogEntry *E = &Snapshot[I];
        if (!E->Valid) continue;

        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "[%02u] tick=%llu pid=%d name=%s type=%s\n",
                      I, E->Tick, E->Pid, E->Name,
                      ExceptionTypeToString(E->Type));

        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "     RIP=0x%llX ERR=0x%llX RSP=0x%llX CS=0x%llX FLG=0x%llX",
                      E->Rip, E->ErrorCode, E->Rsp, E->Cs, E->Rflags);

        if (E->Type == EXC_PAGE_FAULT) {
            BaseconPrintf(BASECON_TYPE_NORMAL, " CR2=0x%llX", E->FaultAddr);
        }
        BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
    }
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}

VOID ExceptionLogDumpLast(VOID) {
    ExceptionLogEntry Last;
    BOOL Have = FALSE;

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    if (GLogCount > 0) {
        UINT32 Start;
        if (GLogCount < EXCEPTION_LOG_SIZE) Start = 0;
        else Start = GLogHead;
        UINT32 Real = (Start + GLogCount - 1) % EXCEPTION_LOG_SIZE;
        Last = GLog[Real];
        Have = Last.Valid;
    }
    SpinLockReleaseIrq(&GLogLock, Flags);

    if (!Have) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "elog: empty\n");
        return;
    }

    const ExceptionLogEntry *E = &Last;
    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "elog: last: tick=%llu pid=%d name=%s type=%s RIP=0x%llX ERR=0x%llX",
                  E->Tick, E->Pid, E->Name,
                  ExceptionTypeToString(E->Type),
                  E->Rip, E->ErrorCode);

    if (E->Type == EXC_PAGE_FAULT) {
        BaseconPrintf(BASECON_TYPE_NORMAL, " CR2=0x%llX", E->FaultAddr);
    }
    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}