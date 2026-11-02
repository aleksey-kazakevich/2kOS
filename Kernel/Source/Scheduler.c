#include <Drivers/Irq/Apic/Apic.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Time/Timer.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <KObj/StdIo.h>
#include <Scheduler.h>
#include <Io/StdErr.h>
#include <Io/StdOut.h>
#include <KObj/KObj.h>
#include <SpinLock.h>
#include <Asm/Cpu.h>
#include <Basecon.h>
#include <PerCpu.h>
#include <Return.h>
#include <Fatal.h>
#include <Tss.h>

_Static_assert(OffsetOf(PerCpu, SyscallStackTop) == 0x30,
               "SyscallHandler.asm: PERCPU_SYSCALL_STACKTOP must be 0x30");
_Static_assert(OffsetOf(PerCpu, SyscallUserRsp)  == 0x38,
               "SyscallHandler.asm: PERCPU_SYSCALL_USERRSP must be 0x38");

/* ============================================================================
 * EXTERNAL
 * ============================================================================ */

EXTERN(UINT8, StackStart);
EXTERN(UINT8, StackEnd);

/* From Smp.c */
EXTERN(VOID, SchedReschedIpiHandler(VOID));

/* ============================================================================
 * CFS: table of weights
 * ============================================================================ */

const UINT32 GSchedWeights[NICE_LEVELS] = {
    88761, 71755, 56483, 46273, 36291,   /* -20..-16 */
    29154, 23254, 18705, 14949, 11916,   /* -15..-11 */
     9548,  7620,  6100,  4904,  3906,   /* -10..-6  */
     3121,  2501,  1991,  1591,  1277,   /*  -5..-1  */
     1024,                                /*   0     */
      820,   655,   526,   423,   335,   /*   1..5   */
      272,   215,   172,   137,   110,   /*   6..10  */
       87,    70,    56,    45,    36,   /*  11..15  */
       29,    23,    18,    15            /*  16..19  */
};

_Static_assert(sizeof(GSchedWeights) / sizeof(GSchedWeights[0]) == NICE_LEVELS,
               "GSchedWeights size mismatch");

/* ============================================================================
 * CONFIG
 * ============================================================================ */

#define SCHED_BALANCE_INTERVAL_MS  100
#define SCHED_BALANCE_THRESHOLD    (512ULL)     /* ~half the weight of nice 0 */
#define SCHED_IDLE_STEAL_MIN_LOAD  (1024ULL)    /* We don't steal from the empty */
#define SCHED_TASK_HOT_MS          5

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */

Task InitTask;
Task *TaskRing = NULLPTR;

STATIC volatile INT NextPid = 1;
STATIC Task *ZombieList = NULLPTR;
STATIC SpinLock GSchedLock;    /* protects global operations: list, kill, create  */

/* Fallback for bootstrap before installing %gs */
STATIC Task *GBootstrapCurrentTask = NULLPTR;
STATIC UINT64 GBootstrapLastAccountTick = 0;
STATIC UINT64 GBootstrapFrac = 0;

/* ============================================================================
 * HELPERS
 * ============================================================================ */

STATIC INLINE PerCpu *TryGetPerCpu(VOID) {
    UINT64 GsBase = ReadMSR(0xC0000101);
    if (GsBase == 0) return NULLPTR;
    return (PerCpu *)GsBase;
}

STATIC INLINE INT CurrentCpuIndex(VOID) {
    PerCpu *P = TryGetPerCpu();
    return P ? (INT)P->CpuIndex : 0;
}

STATIC UINT64 KernelStackTop(Task *T) {
    return ((UINT64)T->KernelStack + T->KernelStackSize) & ~0xFULL;
}

STATIC UINT64 *TaskMm(Task *T) {
    if (T && T->PageTable) return T->PageTable;
    return PagingGetKernelRoot();
}

STATIC VOID FreeKernelStack(VOID *Virt, USIZE Size) {
    if (!Virt || Size == 0) return;
    UINT32 Pages = (UINT32)(PAGE_ALIGN_UP(Size) / PAGE_SIZE);
    PhysAllocFreeRange(PhysAllocGet(), VirtToPhysPtr(Virt), Pages);
}

STATIC VOID FreeTaskResources(Task *T) {
    if (!T || T == &InitTask) return;

    /* Close all KObj handles — drop refcounts, call Close callbacks */
    for (UINT32 I = 0; I < KOBJ_HANDLE_MAX; I++) {
        if (T->KObjHandles[I].Valid && T->KObjHandles[I].Object) {
            KObj *Obj = T->KObjHandles[I].Object;
            T->KObjHandles[I].Valid  = FALSE;
            T->KObjHandles[I].Object = NULLPTR;
            T->KObjHandles[I].Access = 0;
            if (Obj->Ops && Obj->Ops->Close) Obj->Ops->Close(Obj);
            KObjUnref(Obj);
        }
    }

    if (T->StdoutObj)  { KObjUnref(T->StdoutObj);  T->StdoutObj  = NULLPTR; }
    if (T->StderrObj)  { KObjUnref(T->StderrObj);  T->StderrObj  = NULLPTR; }
    if (T->StdwarnObj) { KObjUnref(T->StdwarnObj); T->StdwarnObj = NULLPTR; }
    if (T->StdinObj)   { KObjUnref(T->StdinObj);   T->StdinObj   = NULLPTR; }

    if (T->Cwd) {
        VfsInodeUnref(T->Cwd);
        T->Cwd = NULLPTR;
    }

    FdTableCloseAll(&T->Fds);

    if (T->KernelStack) FreeKernelStack(T->KernelStack, T->KernelStackSize);
    if (T->IsUser && T->PageTable) PagingDestroyUserAddressSpace(T->PageTable);
    MemoryFreeAligned(T);
}

VOID *AllocKernelStack(USIZE Bytes, USIZE *OutSize) {
    USIZE Size = PAGE_ALIGN_UP(Bytes);
    UINT32 Pages = (UINT32)(Size / PAGE_SIZE);
    VOID *Virt = PhysAllocAllocateRange(PhysAllocGet(), Pages);
    if (!Virt) return NULLPTR;
    if (OutSize) *OutSize = Size;
    return Virt;
}

/* ============================================================================
 * PUBLIC: GetCurrentTask
 * ============================================================================ */

Task *GetCurrentTask(VOID) {
    PerCpu *Self = TryGetPerCpu();
    if (Self) return Self->CurrentTask;
    return GBootstrapCurrentTask;
}

STATIC VOID SetCurrentTask(Task *T) {
    PerCpu *Self = TryGetPerCpu();
    if (Self) Self->CurrentTask = T;
    else      GBootstrapCurrentTask = T;
}

/* ============================================================================
 * PER-CPU RUNQUEUE
 *
 * RunQueue is a circular list. The RunQueue points to the TAIL.
 * Tail->RunNext = head.
 * ============================================================================ */

STATIC VOID RqAddLocked(PerCpu *P, Task *T) {
    if (!P || !T) return;
    if (T->OnRunQueue) return;

    /* Equalizing the vruntime */
    if (T->Vruntime < P->MinVruntime) T->Vruntime = P->MinVruntime;

    if (!P->RunQueue) {
        T->RunNext = T;
        P->RunQueue = T;
    } else {
        T->RunNext = P->RunQueue->RunNext;
        P->RunQueue->RunNext = T;
        P->RunQueue = T;
    }

    T->OnRunQueue = TRUE;
    T->Cpu = (INT32)P->CpuIndex;
    P->RunQueueCount++;
    P->Load += T->Weight;
}

STATIC BOOL RqDelLocked(PerCpu *P, Task *T) {
    if (!P || !T || !T->OnRunQueue) return FALSE;

    /* Case 1: The only task */
    if (P->RunQueue == T && T->RunNext == T) {
        P->RunQueue = NULLPTR;
        T->RunNext = NULLPTR;
        T->OnRunQueue = FALSE;
        P->RunQueueCount--;
        P->Load -= T->Weight;
        return TRUE;
    }

    /* Case 2: we are looking in the ring */
    Task *Prev = P->RunQueue;
    Task *It = Prev->RunNext;
    do {
        if (It == T) {
            Prev->RunNext = It->RunNext;
            if (P->RunQueue == T) P->RunQueue = Prev;
            T->RunNext = NULLPTR;
            T->OnRunQueue = FALSE;
            P->RunQueueCount--;
            P->Load -= T->Weight;
            return TRUE;
        }
        Prev = It;
        It = It->RunNext;
    } while (It != P->RunQueue->RunNext);

    return FALSE;
}

STATIC Task *RqPickMinVruntimeLocked(PerCpu *P) {
    if (!P || !P->RunQueue) return NULLPTR;

    Task *Best = NULLPTR;
    UINT64 BestV = UINT64MAX;

    Task *Start = P->RunQueue->RunNext;
    Task *It = Start;
    do {
        if (It->State == TASK_READY && It->OnRunQueue) {
            if (It->Vruntime < BestV) {
                BestV = It->Vruntime;
                Best = It;
            }
        }
        It = It->RunNext;
    } while (It != Start);

    return Best;
}

/* ============================================================================
 * CFS: accounting
 * ============================================================================ */

STATIC VOID SchedAccountCurrentLocked(Task *Cur, UINT64 NowTick) {
    PerCpu *Self = TryGetPerCpu();

    UINT64 *LastPtr;
    UINT64 *FracPtr;

    if (Self) {
        LastPtr = &Self->LastAccountTick;
        FracPtr = &Self->SchedFracRemainder;
    } else {
        LastPtr = &GBootstrapLastAccountTick;
        FracPtr = &GBootstrapFrac;
    }

    UINT64 Last = *LastPtr;
    if (NowTick <= Last) {
        *LastPtr = NowTick;
        return;
    }

    UINT64 Delta = NowTick - Last;
    *LastPtr = NowTick;

    UINT64 Weight = Cur->Weight ? Cur->Weight : NICE_0_WEIGHT;

    UINT64 Num = Delta * NICE_0_WEIGHT + *FracPtr;
    UINT64 DeltaV = Num / Weight;
    *FracPtr = Num % Weight;

    Cur->Vruntime += DeltaV;
    Cur->ExecTicks += Delta;

    if (Self && Cur->Vruntime < Self->MinVruntime) {
        Self->MinVruntime = Cur->Vruntime;
    }
}

/* ============================================================================
 * IDLE LOOP
 * ============================================================================ */

STATIC VOID IdleLoop(VOID) {
    UINT64 GsBase = ReadMSR(0xC0000101);
    if (GsBase < 0xffff800000000000ULL) {
        StdErrPrintf("idle: bad %%gs on AP, gsbase=0x%llX\n", GsBase);
        PerCpu *Fallback = PerCpuGetByLapicId(ApicGetId());
        if (Fallback) {
            WriteMSR(0xC0000101, (UINT64)(UINTPTR)Fallback);
        } else {
            for (;;) Halt();
        }
    }
    PerCpu *Self = TryGetPerCpu();
    for (;;) {
        if (Self) Self->Idle = TRUE;
        LocalInterruptsEnable();
        if (!SchedIdleBalance()) {
            Halt();
        }
        if (Self) Self->Idle = FALSE;
    }
}

/* ============================================================================
 * INIT: global and per-CPU
 * ============================================================================ */

VOID SchedulerInit(VOID) {
    SpinLockInit(&GSchedLock);

    MemSet(&InitTask, 0, sizeof(InitTask));
    InitTask.Pid = 0;
    InitTask.State = TASK_RUNNING;
    MemCpy(InitTask.Name, "INIT", 5);
    InitTask.KernelStack = (VOID *)&StackStart;
    InitTask.KernelStackSize = (USIZE)(&StackEnd - &StackStart);
    InitTask.Next = &InitTask;
    InitTask.RunNext = &InitTask;
    InitTask.PageTable = PagingGetKernelRoot();
    InitTask.IsUser = FALSE;
    InitTask.Nice = NICE_DEFAULT;
    InitTask.Weight = NiceToWeight(NICE_DEFAULT);
    InitTask.Vruntime = 0;
    InitTask.InRing = TRUE;
    InitTask.OnRunQueue = FALSE;   /* let's put it in PRQ in SchedulerInitPerCpu */
    InitTask.Cpu = 0;
    InitTask.CpuAffinity = 0;

    InitTask.StdoutObj  = StdIoGetDefaultConsole();
    InitTask.StderrObj  = StdIoGetDefaultConsole();
    InitTask.StdwarnObj = StdIoGetDefaultConsole();
    InitTask.StdinObj   = NULLPTR;

    FdTableInit(&InitTask.Fds);
    FdSetupStdio(&InitTask.Fds, InitTask.StdinObj,
             InitTask.StdoutObj, InitTask.StderrObj);

    InitTask.Cwd = VfsCurrentDir;
    if (InitTask.Cwd) VfsInodeRef(InitTask.Cwd);

    asm volatile ("fninit");
    asm volatile ("fxsave %0" : "=m"(InitTask.FpuState) :: "memory");

    TaskRing = &InitTask;
    NextPid = 1;
    ZombieList = NULLPTR;
    GBootstrapCurrentTask = NULLPTR;
    GBootstrapLastAccountTick = 0;
    GBootstrapFrac = 0;
}

/* It is called for each CPU after GPerCpu[i] is full. */
VOID SchedulerInitPerCpu(UINT32 CpuIndex) {
    if (CpuIndex >= MAX_CPUS) return;

    PerCpu *P = &GPerCpu[CpuIndex];

    SpinLockInit(&P->RunQueueLock);

    P->RunQueue = NULLPTR;
    P->RunQueueCount = 0;
    P->Load = 0;
    P->MinVruntime = 0;
    P->LastAccountTick = TimerTicks();
    P->SchedFracRemainder = 0;
    P->LastBalanceTick = 0;
    P->Idle = FALSE;
    P->NeedResched = FALSE;
    P->CurrentTask = NULLPTR;

    /* Creating an idle task for this CPU */
    Task *Idle = (Task *)MemoryAllocateAligned(sizeof(Task), 16);
    if (!Idle) {
        StdErrPrintf("sched: cannot allocate idle task for cpu %u\n",
                      CpuIndex);
        return;
    }

    MemSet(Idle, 0, sizeof(Task));
    MemCpy(Idle->FpuState, InitTask.FpuState, sizeof(Idle->FpuState));

    Idle->Pid = -1;
    Idle->State = TASK_READY;
    Idle->Nice = NICE_MAX;
    Idle->Weight = 1;
    Idle->Vruntime = UINT64MAX;
    Idle->ExecTicks = 0;
    Idle->WakeupTick = 0;
    Idle->IsUser = FALSE;
    Idle->PageTable = PagingGetKernelRoot();
    Idle->InRing = FALSE;          /* idle is not in the global ring */
    Idle->OnRunQueue = FALSE;
    Idle->Cpu = (INT32)CpuIndex;
    Idle->CpuAffinity = (1u << CpuIndex);
    StrnCpy(Idle->Name, "idle", sizeof(Idle->Name) - 1);

    USIZE KSize = 0;
    Idle->KernelStack = AllocKernelStack(KSTACK_SIZE, &KSize);
    Idle->KernelStackSize = KSize;

    UINT64 Top = KernelStackTop(Idle);
    Idle->Regs = PrepareInitialStack((UINT64)IdleLoop,
                                     (VOID *)Top, Top, 0, 0, FALSE);

    P->IdleTask = Idle;

    /* BSP: InitTask becomes current, idle — in case of downtime */
    if (P->IsBsp) {
        P->CurrentTask = &InitTask;
        UINTPTR F = SpinLockAcquireIrq(&P->RunQueueLock);
        RqAddLocked(P, &InitTask);
        SpinLockReleaseIrq(&P->RunQueueLock, F);
    }
}

/* ============================================================================
 * TASK CREATE
 * ============================================================================ */

STATIC VOID LinkGlobalRingLocked(Task *T) {
    if (T->InRing) return;
    if (!TaskRing) {
        TaskRing = T;
        T->Next = T;
    } else {
        T->Next = TaskRing->Next;
        TaskRing->Next = T;
        TaskRing = T;
    }
    T->InRing = TRUE;
}

STATIC VOID UnlinkGlobalRingLocked(Task *T) {
    if (!TaskRing || !T || !T->InRing) return;

    if (TaskRing->Next == TaskRing) {
        if (TaskRing == T) {
            TaskRing = &InitTask;
            InitTask.Next = &InitTask;
            InitTask.InRing = TRUE;
            T->InRing = FALSE;
        }
        return;
    }

    Task *Prev = TaskRing;
    Task *It = TaskRing->Next;
    do {
        if (It == T) {
            Prev->Next = It->Next;
            if (TaskRing == It) TaskRing = Prev;
            T->InRing = FALSE;
            return;
        }
        Prev = It;
        It = It->Next;
    } while (It != TaskRing->Next);
}

STATIC VOID TaskInitCfsFields(Task *T) {
    T->Nice = NICE_DEFAULT;
    T->Weight = NiceToWeight(NICE_DEFAULT);
    T->Vruntime = 0;
    T->ExecTicks = 0;
    T->WakeupTick = TimerTicks();
    T->LastRunTick = T->WakeupTick;
    T->InRing = FALSE;
    T->OnRunQueue = FALSE;
    T->Cpu = -1;
    T->CpuAffinity = 0;
}

STATIC VOID InheritStdChannels(Task *T) {
    Task *Parent = GetCurrentTask();
    if (Parent) {
        if (Parent->StdoutObj)  { T->StdoutObj  = Parent->StdoutObj;  KObjRef(T->StdoutObj);  }
        if (Parent->StderrObj)  { T->StderrObj  = Parent->StderrObj;  KObjRef(T->StderrObj);  }
        if (Parent->StdwarnObj) { T->StdwarnObj = Parent->StdwarnObj; KObjRef(T->StdwarnObj); }
        if (Parent->StdinObj)   { T->StdinObj   = Parent->StdinObj;   KObjRef(T->StdinObj);   }
	if (Parent->Cwd) {
            T->Cwd = Parent->Cwd;
            VfsInodeRef(T->Cwd);
        }
    }
    if (!T->StdoutObj)  T->StdoutObj  = StdIoGetDefaultConsole();
    if (!T->StderrObj)  T->StderrObj  = StdIoGetDefaultConsole();
    if (!T->StdwarnObj) T->StdwarnObj = StdIoGetDefaultConsole();
    /* stdin: leave NULL if not inherited */

    if (!T->Cwd && VfsCurrentDir) {
        T->Cwd = VfsCurrentDir;
        VfsInodeRef(T->Cwd);
    }

    FdSetupStdio(&T->Fds, T->StdinObj, T->StdoutObj, T->StderrObj);
} 

INT TaskCreate(VOID (*Entry)(VOID), USIZE StackSize, const CHAR *Name) {
    if (!Entry) return -1;
    if (StackSize == 0) StackSize = KSTACK_SIZE;

    Task *T = (Task *)MemoryAllocateAligned(sizeof(Task), 16);
    if (!T) return -1;

    USIZE KSize = 0;
    VOID *KStack = AllocKernelStack(StackSize, &KSize);
    if (!KStack) {
        MemoryFreeAligned(T);
        return -1;
    }

    MemSet(T, 0, sizeof(Task));
    MemCpy(T->FpuState, InitTask.FpuState, sizeof(T->FpuState));
    T->Pid = (INT)__sync_fetch_and_add(&NextPid, 1);
    T->State = TASK_READY;
    T->KernelStack = KStack;
    T->KernelStackSize = KSize;
    T->PageTable = PagingGetKernelRoot();
    T->IsUser = FALSE;
    StrnCpy(T->Name, Name ? Name : "unknown", sizeof(T->Name) - 1);

    InheritStdChannels(T);

    TaskInitCfsFields(T);

    UINT64 Top = KernelStackTop(T);
    T->Regs = PrepareInitialStack((UINT64)Entry, (VOID *)Top, Top, 0, 0, FALSE);

    /* We register globally */
    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    LinkGlobalRingLocked(T);
    SpinLockReleaseIrq(&GSchedLock, F);

    /* We put a suitable CPU in the PRQ */
    INT TargetCpu = SchedPickCpuForTask(T);
    PerCpu *P = PerCpuGetByIndex((UINT32)TargetCpu);
    if (!P) P = PerCpuGetByIndex(0);

    F = SpinLockAcquireIrq(&P->RunQueueLock);
    RqAddLocked(P, T);
    SpinLockReleaseIrq(&P->RunQueueLock, F);

    return T->Pid;
}

INT UserTaskCreate(const VOID *Image,
                   USIZE ImageSize,
                   USIZE UserStackSize,
                   USIZE KernelStackSize,
                   INT ArgC,
                   UINTPTR ArgvUserPtr,
                   const CHAR *Name) {
    if (!Image || ImageSize == 0) return -1;

    if (UserStackSize == 0) UserStackSize = USER_STACK_DEFAULT;
    if (KernelStackSize == 0) KernelStackSize = KSTACK_SIZE;

    UserStackSize = PAGE_ALIGN_UP(UserStackSize);
    KernelStackSize = PAGE_ALIGN_UP(KernelStackSize);

    if (USER_STACK_TOP <= UserStackSize) return -1;

    Task *T = (Task *)MemoryAllocateAligned(sizeof(Task), 16);
    if (!T) return -1;
    MemSet(T, 0, sizeof(Task));
    MemCpy(T->FpuState, InitTask.FpuState, sizeof(T->FpuState));

    InheritStdChannels(T);

    USIZE KSize = 0;
    VOID *KStack = AllocKernelStack(KernelStackSize, &KSize);
    if (!KStack) {
        MemoryFreeAligned(T);
        return -1;
    }

    UINT64 *Pml4 = PagingCreateUserAddressSpace();
    if (!Pml4) {
        FreeKernelStack(KStack, KSize);
        MemoryFreeAligned(T);
        return -1;
    }

    USIZE CodeSize = PAGE_ALIGN_UP(ImageSize);
    if (PagingMapAnonUser(Pml4, USER_CODE_VADDR, CodeSize,
                          PTE_USER | PTE_WRITABLE) != SUCCESS) {
        PagingDestroyUserAddressSpace(Pml4);
        FreeKernelStack(KStack, KSize);
        MemoryFreeAligned(T);
        return -1;
    }

    if (PagingCopyToUser(Pml4, USER_CODE_VADDR, Image, ImageSize) != SUCCESS) {
        PagingDestroyUserAddressSpace(Pml4);
        FreeKernelStack(KStack, KSize);
        MemoryFreeAligned(T);
        return -1;
    }

    UINT64 StackBase = USER_STACK_TOP - UserStackSize;
    if (PagingMapAnonUser(Pml4, StackBase, UserStackSize,
                          PTE_USER | PTE_WRITABLE | PTE_NO_EXEC) != SUCCESS) {
        PagingDestroyUserAddressSpace(Pml4);
        FreeKernelStack(KStack, KSize);
        MemoryFreeAligned(T);
        return -1;
    }

    T->Pid = (INT)__sync_fetch_and_add(&NextPid, 1);
    T->State = TASK_READY;
    T->KernelStack = KStack;
    T->KernelStackSize = KSize;
    T->PageTable = Pml4;
    T->IsUser = TRUE;
    StrnCpy(T->Name, Name ? Name : "user", sizeof(T->Name) - 1);

    TaskInitCfsFields(T);

    UINT64 HeapStart = USER_CODE_VADDR + CodeSize;
    HeapStart = PAGE_ALIGN_UP(HeapStart);
    if (HeapStart < USER_HEAP_START) HeapStart = USER_HEAP_START;

    T->BrkStart   = HeapStart;
    T->BrkCurrent = HeapStart;
    T->BrkMax     = USER_HEAP_MAX;

    T->Regs = PrepareInitialStack(USER_CODE_VADDR,
                                  (VOID *)KernelStackTop(T),
                                  USER_STACK_TOP,
                                  ArgC, ArgvUserPtr, TRUE);

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    LinkGlobalRingLocked(T);
    SpinLockReleaseIrq(&GSchedLock, F);

    INT TargetCpu = SchedPickCpuForTask(T);
    PerCpu *P = PerCpuGetByIndex((UINT32)TargetCpu);
    if (!P) P = PerCpuGetByIndex(0);

    F = SpinLockAcquireIrq(&P->RunQueueLock);
    RqAddLocked(P, T);
    SpinLockReleaseIrq(&P->RunQueueLock, F);

    return T->Pid;
}

INT UserTaskCreateFromPageTable(UINT64 *Pml4,
                                 UINT64 Entry,
                                 UINT64 UserSp,
                                 UINT64 HeapStart,
                                 const CHAR *Name) {
    if (!Pml4 || !Entry) return -1;

    Task *T = (Task *)MemoryAllocateAligned(sizeof(Task), 16);
    if (!T) return -1;
    MemSet(T, 0, sizeof(Task));
    MemCpy(T->FpuState, InitTask.FpuState, sizeof(T->FpuState));

    USIZE KSize = 0;
    VOID *KStack = AllocKernelStack(KSTACK_SIZE, &KSize);
    if (!KStack) {
        MemoryFreeAligned(T);
        PagingDestroyUserAddressSpace(Pml4);
        return -1;
    }

    T->Pid = (INT)__sync_fetch_and_add(&NextPid, 1);
    T->State = TASK_READY;
    T->KernelStack = KStack;
    T->KernelStackSize = KSize;
    T->PageTable = Pml4;
    T->IsUser = TRUE;
    StrnCpy(T->Name, Name ? Name : "user", sizeof(T->Name) - 1);

    InheritStdChannels(T);

    TaskInitCfsFields(T);

    if (HeapStart == 0) HeapStart = USER_HEAP_START;
    T->BrkStart   = HeapStart;
    T->BrkCurrent = HeapStart;
    T->BrkMax     = USER_HEAP_MAX;

    T->Regs = PrepareInitialStack(Entry,
                                  (VOID *)KernelStackTop(T),
                                  UserSp,
                                  0, 0, TRUE);

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    LinkGlobalRingLocked(T);
    SpinLockReleaseIrq(&GSchedLock, F);

    INT TargetCpu = SchedPickCpuForTask(T);
    PerCpu *P = PerCpuGetByIndex((UINT32)TargetCpu);
    if (!P) P = PerCpuGetByIndex(0);

    F = SpinLockAcquireIrq(&P->RunQueueLock);
    RqAddLocked(P, T);
    SpinLockReleaseIrq(&P->RunQueueLock, F);

    return T->Pid;
}

/* ============================================================================
 * CPU SELECTION
 * ============================================================================ */

INT SchedPickCpuForTask(Task *T) {
    INT Best = 0;
    UINT64 MinLoad = UINT64MAX;

    for (UINT32 I = 0; I < GCpuCount; I++) {
        PerCpu *P = &GPerCpu[I];
        if (!P->Online) continue;
        if (T && T->CpuAffinity && !(T->CpuAffinity & (1u << I))) continue;

        UINT64 L = __atomic_load_n(&P->Load, __ATOMIC_RELAXED);
        if (L < MinLoad) {
            MinLoad = L;
            Best = (INT)I;
        }
    }
    return Best;
}

/* ============================================================================
 * MIGRATION
 * ============================================================================ */

INT SchedMigrateTask(Task *T, INT TargetCpu) {
    if (!T) return -1;
    if (TargetCpu < 0 || TargetCpu >= (INT)GCpuCount) return -1;

    PerCpu *Target = PerCpuGetByIndex((UINT32)TargetCpu);
    if (!Target || !Target->Online) return -1;

    if (T->CpuAffinity && !(T->CpuAffinity & (1u << TargetCpu))) return -1;

    INT OldCpu = T->Cpu;
    if (OldCpu < 0 || OldCpu == TargetCpu) return 0;

    PerCpu *Old = PerCpuGetByIndex((UINT32)OldCpu);
    if (!Old) return -1;

    /* Remove from the old one */
    UINTPTR F = SpinLockAcquireIrq(&Old->RunQueueLock);
    if (!RqDelLocked(Old, T)) {
        SpinLockReleaseIrq(&Old->RunQueueLock, F);
        return -1;
    }
    SpinLockReleaseIrq(&Old->RunQueueLock, F);

    /* Put it in a new one */
    F = SpinLockAcquireIrq(&Target->RunQueueLock);
    RqAddLocked(Target, T);
    SpinLockReleaseIrq(&Target->RunQueueLock, F);

    /* If the task is currently RUNNING on the old CPU, ask for resched */
    if (Old->CurrentTask == T) {
        Old->NeedResched = TRUE;
        /* IPI */
        extern VOID ApicSendIpi(UINT32 ApicId, UINT32 Vector);
        ApicSendIpi(Old->LapicId, SCHED_RESCHED_IPI_VECTOR);
    }

    /* If the target is asleep, wake him up */
    if (Target->Idle) {
        extern VOID ApicSendIpi(UINT32 ApicId, UINT32 Vector);
        ApicSendIpi(Target->LapicId, SCHED_RESCHED_IPI_VECTOR);
    }

    return 0;
}

/* ============================================================================
 * AFFINITY
 * ============================================================================ */

INT SchedSetAffinity(INT Pid, UINT32 Mask) {
    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);

    Task *Found = NULLPTR;
    if (TaskRing) {
        Task *It = TaskRing->Next;
        do {
            if (It->Pid == Pid) { Found = It; break; }
            It = It->Next;
        } while (It != TaskRing->Next);
    }

    if (!Found) {
        SpinLockReleaseIrq(&GSchedLock, F);
        return -1;
    }

    Found->CpuAffinity = Mask;
    SpinLockReleaseIrq(&GSchedLock, F);

    /* If the task is no longer running on a valid CPU, migrate */
    if (Mask && Found->Cpu >= 0 && !(Mask & (1u << Found->Cpu))) {
        for (UINT32 I = 0; I < GCpuCount; I++) {
            if (Mask & (1u << I)) {
                SchedMigrateTask(Found, (INT)I);
                break;
            }
        }
    }

    return 0;
}

UINT32 SchedGetAffinity(INT Pid) {
    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);

    UINT32 Mask = 0;
    if (TaskRing) {
        Task *It = TaskRing->Next;
        do {
            if (It->Pid == Pid) { Mask = It->CpuAffinity; break; }
            It = It->Next;
        } while (It != TaskRing->Next);
    }

    SpinLockReleaseIrq(&GSchedLock, F);
    return Mask;
}

/* ============================================================================
 * LOAD BALANCING
 * ============================================================================ */

/* The order of capturing PRQ locks: the smaller index is the first. */
STATIC VOID LockTwoPrqs(PerCpu *A, PerCpu *B, UINTPTR *Fa, UINTPTR *Fb) {
    if (A == B) {
        *Fa = SpinLockAcquireIrq(&A->RunQueueLock);
        *Fb = 0;
        return;
    }
    if (A->CpuIndex < B->CpuIndex) {
        *Fa = SpinLockAcquireIrq(&A->RunQueueLock);
        *Fb = SpinLockAcquireIrq(&B->RunQueueLock);
    } else {
        *Fb = SpinLockAcquireIrq(&B->RunQueueLock);
        *Fa = SpinLockAcquireIrq(&A->RunQueueLock);
    }
}

STATIC VOID UnlockTwoPrqs(PerCpu *A, PerCpu *B, UINTPTR Fa, UINTPTR Fb) {
    if (A == B) {
        SpinLockReleaseIrq(&A->RunQueueLock, Fa);
        return;
    }
    SpinLockReleaseIrq(&A->RunQueueLock, Fa);
    SpinLockReleaseIrq(&B->RunQueueLock, Fb);
}

/* Trying to remove one task from Victim and put it in Self.
 * Both PRQ locks are taken internally. Returns TRUE on success. */
STATIC BOOL TryStealFrom(PerCpu *Self, PerCpu *Victim) {
    if (!Self || !Victim) return FALSE;
    if (Self == Victim) return FALSE;
    if (!Victim->Online) return FALSE;
    if (Victim->RunQueueCount <= 1) return FALSE;

    UINTPTR Fs, Fv;
    LockTwoPrqs(Self, Victim, &Fs, &Fv);

    /* Re-checking under locks */
    if (Victim->RunQueueCount <= 1 || !Victim->RunQueue) {
        UnlockTwoPrqs(Self, Victim, Fs, Fv);
        return FALSE;
    }

    /* We are looking for a task with the maximum vruntime — it is the cheapest to give it.
     * We do not take the victim's current task (it is RUNNING).
     * We do not accept tasks with affinity that do not include Self. */
    Task *Best = NULLPTR;
    UINT64 BestV = 0;

    Task *Start = Victim->RunQueue->RunNext;
    Task *It = Start;
    do {
        if (It->State != TASK_READY || !It->OnRunQueue) {
            It = It->RunNext;
            continue;
        }
        if (It == Victim->CurrentTask) {
            It = It->RunNext;
            continue;
        }
        if (It->CpuAffinity && !(It->CpuAffinity & (1u << Self->CpuIndex))) {
            It = It->RunNext;
            continue;
        }
        if (It->Vruntime > BestV) {
            BestV = It->Vruntime;
            Best = It;
        }
        It = It->RunNext;
    } while (It != Start);

    if (!Best) {
        UnlockTwoPrqs(Self, Victim, Fs, Fv);
        return FALSE;
    }

    /* We remove it from Victim, put it in Self */
    RqDelLocked(Victim, Best);
    RqAddLocked(Self, Best);

    UnlockTwoPrqs(Self, Victim, Fs, Fv);

    return TRUE;
}

BOOL SchedIdleBalance(VOID) {
    if (GCpuCount <= 1) return FALSE;

    INT MyCpu = CurrentCpuIndex();
    PerCpu *Self = PerCpuGetByIndex((UINT32)MyCpu);
    if (!Self) return FALSE;

    /* Looking for the busiest CPU */
    PerCpu *Busiest = NULLPTR;
    UINT64 MaxLoad = 0;

    for (UINT32 I = 0; I < GCpuCount; I++) {
        PerCpu *P = &GPerCpu[I];
        if (!P->Online || P == Self) continue;
        UINT64 L = __atomic_load_n(&P->Load, __ATOMIC_RELAXED);
        if (L > MaxLoad) {
            MaxLoad = L;
            Busiest = P;
        }
    }

    if (!Busiest) return FALSE;
    if (MaxLoad < SCHED_IDLE_STEAL_MIN_LOAD) return FALSE;

    UINT64 SelfLoad = __atomic_load_n(&Self->Load, __ATOMIC_RELAXED);
    if (MaxLoad <= SelfLoad + SCHED_BALANCE_THRESHOLD) return FALSE;

    return TryStealFrom(Self, Busiest);
}

VOID SchedLoadBalance(VOID) {
    if (GCpuCount <= 1) return;

    INT MyCpu = CurrentCpuIndex();
    PerCpu *Self = PerCpuGetByIndex((UINT32)MyCpu);
    if (!Self) return;

    UINT64 Now = TimerTicks();
    UINT64 Freq = TimerFreq();
    if (Freq == 0) Freq = 1000;
    UINT64 Period = (UINT64)Freq * SCHED_BALANCE_INTERVAL_MS / 1000;
    if (Period == 0) Period = 1;

    if (Now - Self->LastBalanceTick < Period) return;
    Self->LastBalanceTick = Now;

    /* Our workload is already decent — we don't steal */
    UINT64 SelfLoad = __atomic_load_n(&Self->Load, __ATOMIC_RELAXED);
    if (SelfLoad > SCHED_BALANCE_THRESHOLD * 2) return;

    SchedIdleBalance();
}

/* ============================================================================
 * SCHEDULING
 * ============================================================================ */

VOID ScheduleFromIsr(UINT64 *Regs, UINT64 **OutRegsPtr) {
    if (!Regs || !OutRegsPtr) {
        Fatal("ScheduleFromIsr: NULLPTR pointer");
        return;
    }

    PerCpu *Self = TryGetPerCpu();
    Task *Cur = Self ? Self->CurrentTask : GBootstrapCurrentTask;

    /* The first tick */
    if (!Cur) {
        if (Self) {
            if (!Self->IdleTask) {
                *OutRegsPtr = Regs;
                return;
            }
            Self->CurrentTask = Self->IdleTask;
            Self->IdleTask->State = TASK_RUNNING;
            Cur = Self->IdleTask;
            UpdateKernelStack(KernelStackTop(Cur));
        } else {
            InitTask.Regs = (TaskFrame *)Regs;
            InitTask.State = TASK_READY;
            if (!InitTask.PageTable) InitTask.PageTable = PagingGetKernelRoot();
            GBootstrapCurrentTask = &InitTask;
            Cur = &InitTask;
            UpdateKernelStack(KernelStackTop(Cur));
        }
    } else {
        Cur->Regs = (TaskFrame *)Regs;

        /* We return the preempted task to its CPU queue. */
        if (Cur->State == TASK_RUNNING) {
            INT CurCpu = Self ? (INT)Self->CpuIndex : 0;
            PerCpu *CurP = PerCpuGetByIndex((UINT32)CurCpu);
            if (CurP) {
                UINTPTR Fc = SpinLockAcquireIrq(&CurP->RunQueueLock);
                Cur->State = TASK_READY;
                if (!Cur->OnRunQueue) {
                    RqAddLocked(CurP, Cur);
                }
                SpinLockReleaseIrq(&CurP->RunQueueLock, Fc);
            } else {
                Cur->State = TASK_READY;
            }
        }
        /* If Cur->State == TASK_BLOCKED/ZOMBIE — do not touch. */
    }

    asm volatile ("fxsave %0" : "=m"(Cur->FpuState) :: "memory");

    /* Accounting */
    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    UINT64 NowTick = TimerTicks();
    SchedAccountCurrentLocked(Cur, NowTick);
    Cur->LastRunTick = NowTick;
    SpinLockReleaseIrq(&GSchedLock, F);

    /* Selecting the next task (take it off the queue) */
    INT MyCpu = Self ? (INT)Self->CpuIndex : 0;
    PerCpu *P = PerCpuGetByIndex((UINT32)MyCpu);
    Task *Next = NULLPTR;

    if (P) {
        F = SpinLockAcquireIrq(&P->RunQueueLock);
        Next = RqPickMinVruntimeLocked(P);
        if (Next) {
            RqDelLocked(P, Next);
            Next->State = TASK_RUNNING;
        }
        SpinLockReleaseIrq(&P->RunQueueLock, F);
    }

    /* Fallback: idle or INIT */
    if (!Next) {
        if (P && P->IdleTask) {
            Next = P->IdleTask;
            Next->State = TASK_RUNNING;
        } else if (Self == NULLPTR) {
            /* BSP before idle task */
            Next = &InitTask;
            Next->State = TASK_RUNNING;
        } else {
            Next = &InitTask;
            Next->State = TASK_RUNNING;
        }
    }

    /* Load balance (optional, but you can leave it) */
    if (P) {
        UINT64 Now2 = TimerTicks();
        UINT64 Freq = TimerFreq();
        if (Freq == 0) Freq = 1000;
        UINT64 Period = (UINT64)Freq * SCHED_BALANCE_INTERVAL_MS / 1000;
        if (Period == 0) Period = 1;
        if (Now2 - P->LastBalanceTick >= Period) {
            P->LastBalanceTick = Now2;
            if (P->Load < SCHED_BALANCE_THRESHOLD) {
                SchedIdleBalance();
            }
        }
    }

    if (Self) Self->CurrentTask = Next;
    else      GBootstrapCurrentTask = Next;

    asm volatile ("fxrstor %0" :: "m"(Next->FpuState) : "memory");
    *OutRegsPtr = (UINT64 *)Next->Regs;
    UpdateKernelStack(KernelStackTop(Next));
    PagingSwitch(TaskMm(Next));
}


VOID UpdateKernelStack(UINT64 Rsp0) {
    PerCpu *Self = TryGetPerCpu();
    if (Self) {
        TssSetRsp0(Self, Rsp0);
    }
}

TaskFrame *PrepareInitialStack(UINT64 Entry,
                               VOID *KernelStackTopAddr,
                               UINT64 ResumeRsp,
                               INT ArgC,
                               UINTPTR ArgvPtr,
                               BOOL UserMode) {
    UINTPTR Top = ((UINTPTR)KernelStackTopAddr) & ~0xFULL;
    TaskFrame *Frame = (TaskFrame *)(Top - sizeof(TaskFrame));
    MemSet(Frame, 0, sizeof(TaskFrame));

    Frame->Rdi = (UINT64)ArgC;
    Frame->Rsi = (UINT64)ArgvPtr;
    Frame->Rip = Entry;
    Frame->Cs = UserMode ? USER_CS : KERNEL_CS;
    Frame->Rflags = X86_EFLAGS_IF | X86_EFLAGS_FIXED;
    if (UserMode) {
        Frame->Rsp = ResumeRsp;           /* argv/argc on stack, _start reads them */
    } else {
        Frame->Rsp = (ResumeRsp & ~0xFULL) - 8;   /* simulate 'call' — 8 mod 16 */
    }
    Frame->Ss = UserMode ? USER_SS : KERNEL_SS;

    return Frame;
}

/* ============================================================================
 * TASK LIST / ALIVE
 * ============================================================================ */

INT TaskList(TaskInfo *Buf, USIZE Max) {
    if (!Buf) return -1;

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    if (!TaskRing) { SpinLockReleaseIrq(&GSchedLock, F); return 0; }

    INT Count = 0;
    Task *It = TaskRing->Next;
    do {
        if (Count >= (INT)Max) break;
        Buf[Count].Pid = It->Pid;
        Buf[Count].State = It->State;
        StrnCpy(Buf[Count].Name, It->Name, sizeof(Buf[Count].Name) - 1);
        Count++;
        It = It->Next;
    } while (It != TaskRing->Next);

    SpinLockReleaseIrq(&GSchedLock, F);
    return Count;
}

INT TaskListEx(TaskInfoEx *Buf, USIZE Max) {
    if (!Buf) return -1;

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    if (!TaskRing) { SpinLockReleaseIrq(&GSchedLock, F); return 0; }

    INT Count = 0;
    Task *It = TaskRing->Next;
    do {
        if (Count >= (INT)Max) break;
        Buf[Count].Pid = It->Pid;
        Buf[Count].State = It->State;
        StrnCpy(Buf[Count].Name, It->Name, sizeof(Buf[Count].Name) - 1);
        Buf[Count].Nice = It->Nice;
        Buf[Count].Weight = It->Weight;
        Buf[Count].Vruntime = It->Vruntime;
        Buf[Count].ExecTicks = It->ExecTicks;
        Buf[Count].Cpu = It->Cpu;
        Count++;
        It = It->Next;
    } while (It != TaskRing->Next);

    SpinLockReleaseIrq(&GSchedLock, F);
    return Count;
}

BOOL TaskIsAlive(int Pid) {
    if (Pid < 0) return FALSE;
    if (Pid == 0) return TRUE;

    BOOL Alive = FALSE;

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    if (TaskRing) {
        Task *It = TaskRing->Next;
        do {
            if (It->Pid == Pid) {
                Alive = (It->State != TASK_ZOMBIE);
                break;
            }
            It = It->Next;
        } while (It != TaskRing->Next);
    }
    SpinLockReleaseIrq(&GSchedLock, F);

    if (!Alive) ReapZombies();
    return Alive;
}

/* ============================================================================
 * KILL / EXIT
 * ============================================================================ */

STATIC VOID AddToZombieListLocked(Task *T) {
    if (!T) return;
    T->ZombieNext = ZombieList;
    ZombieList = T;
}

INT SchedulerKill(int Pid) {
    if (Pid <= 0) return -1;

    ReapZombies();

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);

    if (!TaskRing) { SpinLockReleaseIrq(&GSchedLock, F); return -1; }

    Task *Found = NULLPTR;
    Task *It = TaskRing->Next;
    do {
        if (It->Pid == Pid) { Found = It; break; }
        It = It->Next;
    } while (It != TaskRing->Next);

    if (!Found || Found == &InitTask) {
        SpinLockReleaseIrq(&GSchedLock, F);
        return -1;
    }

    PerCpu *Self = TryGetPerCpu();
    Task *Cur = Self ? Self->CurrentTask : GBootstrapCurrentTask;
    if (Found->State == TASK_RUNNING && Found != Cur) {
        SpinLockReleaseIrq(&GSchedLock, F);
        return -1;
    }

    BOOL IsCurrent = (Found == Cur);

    Found->State = TASK_ZOMBIE;
    AddToZombieListLocked(Found);
    UnlinkGlobalRingLocked(Found);

    /* Withdraw from PRQ if there is */
    INT Cpu = Found->Cpu;
    if (Cpu >= 0) {
        PerCpu *P = PerCpuGetByIndex((UINT32)Cpu);
        if (P) {
            UINTPTR F2 = SpinLockAcquireIrq(&P->RunQueueLock);
            RqDelLocked(P, Found);
            SpinLockReleaseIrq(&P->RunQueueLock, F2);
        }
    }

    SpinLockReleaseIrq(&GSchedLock, F);

    if (IsCurrent) {
        for (;;) {
            LocalInterruptsEnable();
            Halt();
        }
    }

    FreeTaskResources(Found);
    return 0;
}

INT SchedulerKillCurrent(VOID) {
    Task *Cur = GetCurrentTask();
    if (!Cur || Cur == &InitTask) return -1;
    return SchedulerKill(Cur->Pid);
}

INT TaskStop(int Pid) {
    return SchedulerKill(Pid);
}

VOID TaskExit(int ExitCode) {
    ReapZombies();

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);

    PerCpu *Self = TryGetPerCpu();
    Task *Cur = Self ? Self->CurrentTask : GBootstrapCurrentTask;

    if (!Cur || Cur == &InitTask) {
        SpinLockReleaseIrq(&GSchedLock, F);
        return;
    }

    Cur->ExitCode = ExitCode;
    Cur->State = TASK_ZOMBIE;
    AddToZombieListLocked(Cur);
    UnlinkGlobalRingLocked(Cur);

    INT Cpu = Cur->Cpu;
    if (Cpu >= 0) {
        PerCpu *P = PerCpuGetByIndex((UINT32)Cpu);
        if (P) {
            UINTPTR F2 = SpinLockAcquireIrq(&P->RunQueueLock);
            RqDelLocked(P, Cur);
            SpinLockReleaseIrq(&P->RunQueueLock, F2);
        }
    }

    SpinLockReleaseIrq(&GSchedLock, F);

    for (;;) {
        LocalInterruptsEnable();
        Halt();
    }
}

VOID ReapZombies(VOID) {
    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    Task *Z = ZombieList;
    ZombieList = NULLPTR;
    SpinLockReleaseIrq(&GSchedLock, F);

    while (Z) {
        Task *NextZ = Z->ZombieNext;
        FreeTaskResources(Z);
        Z = NextZ;
    }
}

/* ============================================================================
 * BLOCK / WAKEUP
 * ============================================================================ */

VOID TaskBlock(VOID) {
    Task *Cur = GetCurrentTask();
    if (!Cur || Cur == &InitTask) return;

    PerCpu *Self = TryGetPerCpu();
    INT MyCpu = Self ? (INT)Self->CpuIndex : 0;
    PerCpu *P = PerCpuGetByIndex((UINT32)MyCpu);

    if (P) {
        UINTPTR F = SpinLockAcquireIrq(&P->RunQueueLock);
        Cur->State = TASK_BLOCKED;
        if (Cur->OnRunQueue) RqDelLocked(P, Cur);
        SpinLockReleaseIrq(&P->RunQueueLock, F);
    } else {
        Cur->State = TASK_BLOCKED;
        Cur->OnRunQueue = FALSE;
    }

    while (1) {
        /* The state changes in TaskWakeup. Reading without lock is enough. */
        if (Cur->State != TASK_BLOCKED) break;
        LocalInterruptsEnable();
        Halt();
    }
}

VOID TaskWakeup(Task *T) {
    if (!T) return;

    /* Where should I put it? If the task was on the CPU and it is online, go there.
     * Otherwise— at least-loaded. */
    PerCpu *Target = NULLPTR;

    if (T->Cpu >= 0 && T->Cpu < (INT)GCpuCount) {
        PerCpu *Prev = &GPerCpu[T->Cpu];
        if (Prev->Online &&
            (!T->CpuAffinity || (T->CpuAffinity & (1u << Prev->CpuIndex)))) {
            Target = Prev;
        }
    }

    if (!Target) {
        /* Looking for least-loaded */
        UINT64 MinLoad = UINT64MAX;
        for (UINT32 I = 0; I < GCpuCount; I++) {
            PerCpu *P = &GPerCpu[I];
            if (!P->Online) continue;
            if (T->CpuAffinity && !(T->CpuAffinity & (1u << I))) continue;
            UINT64 L = __atomic_load_n(&P->Load, __ATOMIC_RELAXED);
            if (L < MinLoad) { MinLoad = L; Target = P; }
        }
    }

    if (!Target) Target = PerCpuGetByIndex(0);
    if (!Target) return;

    UINTPTR F = SpinLockAcquireIrq(&Target->RunQueueLock);
    if (T->State == TASK_BLOCKED) {
        T->State = TASK_READY;
        RqAddLocked(Target, T);
        T->WakeupTick = TimerTicks();
    }
    SpinLockReleaseIrq(&Target->RunQueueLock, F);

    /* If the target CPU is idle, wake it up */
    if (Target->Idle) {
        extern VOID ApicSendIpi(UINT32 ApicId, UINT32 Vector);
        ApicSendIpi(Target->LapicId, SCHED_RESCHED_IPI_VECTOR);
    }
}

/* ============================================================================
 * CFS: Priorities
 * ============================================================================ */

INT TaskSetNice(INT Pid, INT Nice) {
    if (Nice < NICE_MIN) Nice = NICE_MIN;
    if (Nice > NICE_MAX) Nice = NICE_MAX;
    if (Pid < 0) return -1;

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);

    if (!TaskRing) { SpinLockReleaseIrq(&GSchedLock, F); return -1; }

    Task *Found = NULLPTR;
    Task *It = TaskRing->Next;
    do {
        if (It->Pid == Pid) { Found = It; break; }
        It = It->Next;
    } while (It != TaskRing->Next);

    if (!Found) { SpinLockReleaseIrq(&GSchedLock, F); return -1; }

    UINT32 OldW = Found->Weight;
    UINT32 NewW = NiceToWeight(Nice);

    /* We update the CPU Load if the task is in the queue */
    if (Found->OnRunQueue && Found->Cpu >= 0) {
        PerCpu *P = PerCpuGetByIndex((UINT32)Found->Cpu);
        if (P) {
            UINTPTR F2 = SpinLockAcquireIrq(&P->RunQueueLock);
            P->Load = (P->Load >= OldW) ? (P->Load - OldW + NewW) : NewW;
            SpinLockReleaseIrq(&P->RunQueueLock, F2);
        }
    }

    Found->Nice = Nice;
    Found->Weight = NewW;

    SpinLockReleaseIrq(&GSchedLock, F);
    return 0;
}

INT TaskGetNice(INT Pid) {
    if (Pid < 0) return NICE_DEFAULT;

    UINTPTR F = SpinLockAcquireIrq(&GSchedLock);
    INT Result = NICE_DEFAULT;
    if (TaskRing) {
        Task *It = TaskRing->Next;
        do {
            if (It->Pid == Pid) { Result = It->Nice; break; }
            It = It->Next;
        } while (It != TaskRing->Next);
    }
    SpinLockReleaseIrq(&GSchedLock, F);
    return Result;
}

VOID TaskSetNiceSelf(INT Nice) {
    Task *Cur = GetCurrentTask();
    if (!Cur || Cur == &InitTask) return;
    TaskSetNice(Cur->Pid, Nice);
}

/* ============================================================================
 * USER SPACE HELPERS
 * ============================================================================ */

BOOL IsAddressInUserSpace(UINT64 Addr) {
    return (Addr >= USER_CODE_VADDR && Addr < 0x00007FFFFFFFFFFFULL);
}

BOOL IsCurrentFromUser(VOID) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return FALSE;
    return Cur->IsUser;
}

/* ============================================================================
 * DIAGNOSTICS
 * ============================================================================ */

VOID SchedDumpRunQueues(VOID) {
    for (UINT32 I = 0; I < GCpuCount; I++) {
        PerCpu *P = &GPerCpu[I];
        UINTPTR F = SpinLockAcquireIrq(&P->RunQueueLock);
        UINT32 N = P->RunQueueCount;
        UINT64 L = P->Load;
        UINT64 MinV = P->MinVruntime;
        SpinLockReleaseIrq(&P->RunQueueLock, F);

        StdOutPrintf("cpu%u: online=%u idle=%u n=%u load=%llu minv=%llu\n",
                      I, P->Online, P->Idle, N, L, MinV);
    }
}