#include <Utils/SyscallLog.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Io/StdOut.h>
#include <Scheduler.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <Return.h>

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

    UINT64 Tick = TimerTicks();
    Task *Current = GetCurrentTask();
    INT Pid = Current ? Current->Pid : -1;
    CHAR Name[32];
    if (Current) {
        StrnCpy(Name, Current->Name, sizeof(Name) - 1);
        Name[sizeof(Name) - 1] = '\0';
    } else {
        StrCpy(Name, "<none>");
    }

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    UINT32 Idx = GLogHead;
    SyscallLogEntry *E = &GLog[Idx];
    E->Tick = Tick;
    E->Pid = Pid;
    StrnCpy(E->Name, Name, sizeof(E->Name) - 1);
    E->Name[sizeof(E->Name) - 1] = '\0';
    E->Num = Num;
    for (INT I = 0; I < 6; I++) E->Args[I] = Args ? Args[I] : 0;
    E->Ret = Ret;
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

    StdOutPrintf("\nSyscall Log (%u entries, total %u)\n",
                  Count, Total);

    if (Count == 0) {
        StdOutPrintf("  (empty)\n\n");
        return;
    }

    for (UINT32 I = 0; I < Count; I++) {
        const SyscallLogEntry *E = &Snapshot[I];
        if (!E->Valid) continue;

        StdOutPrintf("[%03u] tick=%llu pid=%d name=%s num=%llu (%s) arg1=0x%llX arg2=0x%llX arg3=0x%llX ret=%lld\n",
                      I, E->Tick, E->Pid, E->Name, E->Num,
                      SyscallName(E->Num),
                      E->Args[0], E->Args[1], E->Args[2], (INT64)E->Ret);
    }

    StdOutPrintf("\n");
}

INT SyscallLogCopyToUser(SyscallLogEntry *UserBuf, UINT32 MaxEntries) {
    if (!UserBuf || MaxEntries == 0) RETURN(INCORRECT_VALUE);

    SyscallLogEntry Snapshot[SYSCALL_LOG_SIZE];
    UINT32 Count, Total;

    UINTPTR Flags = SpinLockAcquireIrq(&GLogLock);
    Count = GLogCount;
    Total = GLogTotal;
    UINT32 Start = (Count < SYSCALL_LOG_SIZE) ? 0 : GLogHead;
    for (UINT32 I = 0; I < Count; I++) {
        UINT32 Real = (Start + I) % SYSCALL_LOG_SIZE;
        Snapshot[I] = GLog[Real];
    }
    SpinLockReleaseIrq(&GLogLock, Flags);

    if (Count == 0) RETURN(SUCCESS);
    if (Count > MaxEntries) Count = MaxEntries;
    USIZE Bytes = (USIZE)Count * sizeof(SyscallLogEntry);

    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->PageTable) RETURN(NO_OBJECT);

    INT Rc = PagingCopyToUser(Cur->PageTable, (UINT64)(UINTPTR)UserBuf,
                              Snapshot, Bytes);
    if (Rc != SUCCESS) RETURN(Rc);

    return (INT)Bytes;
}