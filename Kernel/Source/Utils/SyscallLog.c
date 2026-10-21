#include <Utils/SyscallLog.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <Basecon.h>

STATIC SyscallLogEntry GLog[SYSCALL_LOG_SIZE];
STATIC volatile UINT32 GLogHead = 0;
STATIC volatile UINT32 GLogCount = 0;
STATIC volatile UINT32 GLogTotal = 0;
STATIC volatile BOOL   GEnabled = TRUE;
STATIC SpinLock GLogLock;

VOID SyscallLogInit(VOID) {
    SpinLockInit(&GLogLock);
    MemSet(GLog, 0, sizeof(GLog));
    GLogHead = 0;
    GLogCount = 0;
    GLogTotal = 0;
    GEnabled = TRUE;
}

VOID SyscallLogSetEnabled(BOOL Enabled) {
    GEnabled = Enabled;
}

BOOL SyscallLogIsEnabled(VOID) {
    return GEnabled;
}

INT SyscallLogPush(UINT64 Num, UINT64 *Args, UINT64 Ret) {
    if (!GEnabled) return -1;

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);

    UINT32 Idx = GLogHead;
    SyscallLogEntry *E = &GLog[Idx];

    E->Tick = TimerTicks();
    E->Num = Num;
    for (INT I = 0; I < 6; I++) {
        E->Args[I] = Args ? Args[I] : 0;
    }
    E->Ret = Ret;

    Task *Current = GetCurrentTask();
    if (Current) {
        E->Pid = Current->Pid;
        StrnCpy(E->Name, Current->Name, sizeof(E->Name) - 1);
        E->Name[sizeof(E->Name) - 1] = '\0';
    } else {
        E->Pid = -1;
        StrCpy(E->Name, "<none>");
    }

    E->Valid = TRUE;

    GLogHead = (GLogHead + 1) % SYSCALL_LOG_SIZE;
    if (GLogCount < SYSCALL_LOG_SIZE) GLogCount++;
    GLogTotal++;

    SpinLockReleaseIrq(&GLogLock, Flags);
    return (INT)Idx;
}

VOID SyscallLogClear(VOID) {
    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    MemSet(GLog, 0, sizeof(GLog));
    GLogHead = 0;
    GLogCount = 0;
    GLogTotal = 0;
    SpinLockReleaseIrq(&GLogLock, Flags);
}

UINT32 SyscallLogCount(VOID) {
    return GLogCount;
}

const SyscallLogEntry* SyscallLogGet(UINT32 Index) {
    if (Index >= GLogCount) return NULLPTR;
    UINT32 Start;
    if (GLogCount < SYSCALL_LOG_SIZE) Start = 0;
    else Start = GLogHead;
    UINT32 Real = (Start + Index) % SYSCALL_LOG_SIZE;
    if (!GLog[Real].Valid) return NULLPTR;
    return &GLog[Real];
}

STATIC const CHAR* SyscallName(UINT64 Num) {
    switch (Num) {
        case 1:    return "write";
        case 12:   return "brk";
        case 60:   return "exit";
        case 39:   return "getpid";
        case 1001: return "getticks";
        default:   return "?";
    }
}

VOID SyscallLogDump(VOID) {
    // Lock-in snapshot, out-of-lock print
    SyscallLogEntry Snapshot[SYSCALL_LOG_SIZE];
    UINT32 Count;
    UINT32 Total;

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    Count = GLogCount;
    Total = GLogTotal;

    UINT32 Start;
    if (Count < SYSCALL_LOG_SIZE) Start = 0;
    else Start = GLogHead;

    for (UINT32 I = 0; I < Count; I++) {
        UINT32 Real = (Start + I) % SYSCALL_LOG_SIZE;
        Snapshot[I] = GLog[Real];
    }

    SpinLockReleaseIrq(&GLogLock, Flags);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "\nSyscall Log (%u entries, total %u)\n",
                  Count, Total);

    if (Count == 0) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "  (empty)\n\n");
        return;
    }

    for (UINT32 I = 0; I < Count; I++) {
        const SyscallLogEntry *E = &Snapshot[I];
        if (!E->Valid) continue;

        BaseconPrintf(BASECON_TYPE_NORMAL,
                      "[%03u] tick=%llu pid=%d name=%s num=%llu (%s) arg1=0x%llX arg2=0x%llX arg3=0x%llX ret=%lld\n",
                      I, E->Tick, E->Pid, E->Name, E->Num,
                      SyscallName(E->Num),
                      E->Args[0], E->Args[1], E->Args[2], (INT64)E->Ret);
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "\n");
}