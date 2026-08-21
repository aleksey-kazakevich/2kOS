#include <Scheduler.h>
#include <Lib/String.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Mem/Paging.h>
#include <Fatal.h>
#include <Asm/Cpu.h>
#include <Tss.h>
#include <Return.h>

EXTERN(UINT8, StackStart);
EXTERN(UINT8, StackEnd);

Task InitTask;
Task *TaskRing = NULLPTR;
static Task *CurrentTask = NULLPTR;
static int NextPid = 1;
static Task *ZombieList = NULLPTR;

static VOID AddToZombieList(Task *T) {
    if (!T)
        return;
    T->ZombieNext = ZombieList;
    ZombieList = T;
}

static INT UnlinkFromRing(Task *T) {
    if (!TaskRing || !T)
        return -1;

    if (TaskRing->Next == TaskRing) {
        if (TaskRing == T) {
            if (&InitTask == T)
                return -1;
            TaskRing = &InitTask;
            InitTask.Next = &InitTask;
            return 0;
        }
        return -1;
    }

    Task *Prev = TaskRing;
    Task *It = TaskRing->Next;
    do {
        if (It == T) {
            Prev->Next = It->Next;
            if (TaskRing == It)
                TaskRing = Prev;
            return 0;
        }
        Prev = It;
        It = It->Next;
    } while (It != TaskRing->Next);

    return -1;
}

VOID *AllocKernelStack(USIZE Bytes, USIZE *OutSize) {
    USIZE Size = PAGE_ALIGN_UP(Bytes);
    UINT32 Pages = (UINT32)(Size / PAGE_SIZE);
    VOID *Virt = PhysAllocAllocateRange(PhysAllocGet(), Pages);
    if (!Virt)
        return NULLPTR;
    if (OutSize)
        *OutSize = Size;
    return Virt;
}

static VOID FreeKernelStack(VOID *Virt, USIZE Size) {
    if (!Virt || Size == 0)
        return;
    UINT32 Pages = (UINT32)(PAGE_ALIGN_UP(Size) / PAGE_SIZE);
    PhysAllocFreeRange(PhysAllocGet(), VirtToPhysPtr(Virt), Pages);
}

static VOID InsertTask(Task *T) {
    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();
    if (!TaskRing) {
        TaskRing = T;
        T->Next = T;
    } else {
        T->Next = TaskRing->Next;
        TaskRing->Next = T;
        TaskRing = T;
    }
    RestoreFlags(Flags);
}

static VOID FreeTaskResources(Task *T) {
    if (!T || T == &InitTask)
        return;

    if (T->KernelStack)
        FreeKernelStack(T->KernelStack, T->KernelStackSize);

    if (T->IsUser && T->PageTable)
        PagingDestroyUserAddressSpace(T->PageTable);

    MemoryFree(T);
}

static Task *PickNext(VOID) {
    if (!TaskRing)
        return NULLPTR;

    Task *Start = CurrentTask ? CurrentTask->Next : TaskRing->Next;
    Task *It = Start;
    do {
        if (It->State == TASK_READY || It->State == TASK_RUNNING)
            return It;
        It = It->Next;
    } while (It != Start);

    return NULLPTR;
}

static UINT64 KernelStackTop(Task *T) {
    return ((UINT64)T->KernelStack + T->KernelStackSize) & ~0xFULL;
}

static UINT64 *TaskMm(Task *T) {
    if (T && T->PageTable)
        return T->PageTable;
    return PagingGetKernelRoot();
}

VOID SchedulerInit(VOID) {
    MemSet(&InitTask, 0, sizeof(InitTask));
    InitTask.Pid = 0;
    InitTask.State = TASK_RUNNING;
    MemCpy(InitTask.Name, "INIT", 5);
    InitTask.Regs = NULLPTR;
    InitTask.KernelStack = (VOID *)&StackStart;
    InitTask.KernelStackSize = (USIZE)(&StackEnd - &StackStart);
    InitTask.Next = &InitTask;
    InitTask.PageTable = PagingGetKernelRoot();
    InitTask.IsUser = FALSE;

    TaskRing = &InitTask;
    CurrentTask = NULLPTR;
    NextPid = 1;
}

INT TaskCreate(VOID (*Entry)(VOID), USIZE StackSize, const CHAR *Name) {
    if (!Entry)
        return -1;

    if (StackSize == 0)
        StackSize = KSTACK_SIZE;

    Task *T = (Task *)MemoryAllocate(sizeof(Task));
    if (!T)
        return -1;

    USIZE KSize = 0;
    VOID *KStack = AllocKernelStack(StackSize, &KSize);
    if (!KStack) {
        MemoryFree(T);
        return -1;
    }

    MemSet(T, 0, sizeof(Task));
    T->Pid = NextPid++;
    T->State = TASK_READY;
    T->KernelStack = KStack;
    T->KernelStackSize = KSize;
    T->PageTable = PagingGetKernelRoot();
    T->IsUser = FALSE;
    StrnCpy(T->Name, Name ? Name : "unknown", sizeof(T->Name) - 1);

    UINT64 Top = KernelStackTop(T);
    T->Regs = PrepareInitialStack((UINT64)Entry,
                                  (VOID *)Top,
                                  Top,
                                  0,
                                  0,
                                  FALSE);

    InsertTask(T);
    return T->Pid;
}

INT UserTaskCreate(const VOID *Image,
                   USIZE ImageSize,
                   USIZE UserStackSize,
                   USIZE KernelStackSize,
                   INT ArgC,
                   UINTPTR ArgvUserPtr,
                   const CHAR *Name) {
    if (!Image || ImageSize == 0)
        return -1;

    if (UserStackSize == 0)
        UserStackSize = USER_STACK_DEFAULT;
    if (KernelStackSize == 0)
        KernelStackSize = KSTACK_SIZE;

    UserStackSize = PAGE_ALIGN_UP(UserStackSize);
    KernelStackSize = PAGE_ALIGN_UP(KernelStackSize);

    if (USER_STACK_TOP <= UserStackSize)
        return -1;

    Task *T = (Task *)MemoryAllocate(sizeof(Task));
    if (!T)
        return -1;
    MemSet(T, 0, sizeof(Task));

    USIZE KSize = 0;
    VOID *KStack = AllocKernelStack(KernelStackSize, &KSize);
    if (!KStack) {
        MemoryFree(T);
        return -1;
    }

    UINT64 *Pml4 = PagingCreateUserAddressSpace();
    if (!Pml4) {
        FreeKernelStack(KStack, KSize);
        MemoryFree(T);
        return -1;
    }

    USIZE CodeSize = PAGE_ALIGN_UP(ImageSize);
    if (PagingMapAnonUser(Pml4, USER_CODE_VADDR, CodeSize,
                          PTE_USER | PTE_WRITABLE) != SUCCESS) {
        PagingDestroyUserAddressSpace(Pml4);
        FreeKernelStack(KStack, KSize);
        MemoryFree(T);
        return -1;
    }

    if (PagingCopyToUser(Pml4, USER_CODE_VADDR, Image, ImageSize) != SUCCESS) {
        PagingDestroyUserAddressSpace(Pml4);
        FreeKernelStack(KStack, KSize);
        MemoryFree(T);
        return -1;
    }

    UINT64 StackBase = USER_STACK_TOP - UserStackSize;
    if (PagingMapAnonUser(Pml4, StackBase, UserStackSize,
                          PTE_USER | PTE_WRITABLE | PTE_NO_EXEC) != SUCCESS) {
        PagingDestroyUserAddressSpace(Pml4);
        FreeKernelStack(KStack, KSize);
        MemoryFree(T);
        return -1;
    }

    T->Pid = NextPid++;
    T->State = TASK_READY;
    T->KernelStack = KStack;
    T->KernelStackSize = KSize;
    T->PageTable = Pml4;
    T->IsUser = TRUE;
    StrnCpy(T->Name, Name ? Name : "user", sizeof(T->Name) - 1);

    T->Regs = PrepareInitialStack(USER_CODE_VADDR,
                                  (VOID *)KernelStackTop(T),
                                  USER_STACK_TOP,
                                  ArgC,
                                  ArgvUserPtr,
                                  TRUE);

    InsertTask(T);
    return T->Pid;
}

INT TaskList(TaskInfo *Buf, USIZE Max) {
    if (!Buf)
        return -1;

    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();

    if (!TaskRing) {
        RestoreFlags(Flags);
        return 0;
    }

    INT Count = 0;
    Task *It = TaskRing->Next;
    do {
        if (Count >= (INT)Max)
            break;
        Buf[Count].Pid = It->Pid;
        Buf[Count].State = It->State;
        StrnCpy(Buf[Count].Name, It->Name, sizeof(Buf[Count].Name) - 1);
        Count++;
        It = It->Next;
    } while (It != TaskRing->Next);

    RestoreFlags(Flags);
    return Count;
}

INT TaskStop(int Pid) {
    if (Pid == 0)
        return -1;

    ReapZombies();

    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();

    if (!TaskRing) {
        RestoreFlags(Flags);
        return -1;
    }

    Task *Found = NULLPTR;
    Task *It = TaskRing->Next;
    do {
        if (It->Pid == Pid) {
            Found = It;
            break;
        }
        It = It->Next;
    } while (It != TaskRing->Next);

    if (!Found) {
        RestoreFlags(Flags);
        return -1;
    }

    if (Found == CurrentTask) {
        CurrentTask->State = TASK_ZOMBIE;
        AddToZombieList(CurrentTask);
        UnlinkFromRing(CurrentTask);
        RestoreFlags(Flags);
        for (;;) {
            LocalInterruptsEnable();
            Halt();
        }
    }

    UnlinkFromRing(Found);
    RestoreFlags(Flags);
    FreeTaskResources(Found);
    return 0;
}

VOID TaskExit(int ExitCode) {
    ReapZombies();

    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();

    if (!CurrentTask || CurrentTask == &InitTask) {
        RestoreFlags(Flags);
        return;
    }

    CurrentTask->ExitCode = ExitCode;
    CurrentTask->State = TASK_ZOMBIE;
    AddToZombieList(CurrentTask);
    UnlinkFromRing(CurrentTask);
    RestoreFlags(Flags);

    for (;;) {
        LocalInterruptsEnable();
        Halt();
    }
}

VOID ReapZombies(VOID) {
    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();
    Task *Z = ZombieList;
    ZombieList = NULLPTR;
    RestoreFlags(Flags);

    while (Z) {
        Task *NextZ = Z->ZombieNext;
        UINTPTR Flags2 = SaveFlags();
        LocalInterruptsDisable();
        UnlinkFromRing(Z);
        RestoreFlags(Flags2);
        FreeTaskResources(Z);
        Z = NextZ;
    }
}

VOID ScheduleFromIsr(UINT64 *Regs, UINT64 **OutRegsPtr) {
    if (!Regs || !OutRegsPtr) {
        Fatal("ScheduleFromIsr: NULLPTR pointer");
        return;
    }

    if (!CurrentTask) {
        InitTask.Regs = (TaskFrame *)Regs;
        InitTask.State = TASK_RUNNING;
        if (!InitTask.PageTable)
            InitTask.PageTable = PagingGetKernelRoot();
        CurrentTask = &InitTask;
        UpdateKernelStack(KernelStackTop(CurrentTask));
    } else {
        CurrentTask->Regs = (TaskFrame *)Regs;
        if (CurrentTask->State == TASK_RUNNING)
            CurrentTask->State = TASK_READY;
    }

    Task *Next = PickNext();
    if (!Next) {
        Fatal("ScheduleFromIsr: no runnable tasks");
        return;
    }

    CurrentTask = Next;
    CurrentTask->State = TASK_RUNNING;
    *OutRegsPtr = (UINT64 *)CurrentTask->Regs;
    UpdateKernelStack(KernelStackTop(CurrentTask));
    PagingSwitch(TaskMm(CurrentTask));
}

Task *GetCurrentTask(VOID) {
    return CurrentTask;
}

BOOL TaskIsAlive(int Pid) {
    if (Pid < 0)
        return FALSE;
    if (Pid == 0)
        return TRUE;

    ReapZombies();

    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();

    if (!TaskRing) {
        RestoreFlags(Flags);
        return FALSE;
    }

    Task *It = TaskRing->Next;
    do {
        if (It->Pid == Pid) {
            BOOL Alive = (It->State != TASK_ZOMBIE);
            RestoreFlags(Flags);
            return Alive;
        }
        It = It->Next;
    } while (It != TaskRing->Next);

    RestoreFlags(Flags);
    return FALSE;
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
    Frame->Rsp = ResumeRsp & ~0xFULL;
    Frame->Ss = UserMode ? USER_SS : KERNEL_SS;

    return Frame;
}

VOID UpdateKernelStack(UINT64 Rsp0) {
    TssSetRsp0(Rsp0);
}

INT SchedulerKill(int Pid) {
    if (Pid == 0 || Pid < 0) return -1;
    if (!TaskRing) return -1;
    
    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();
    
    Task *Found = NULLPTR;
    Task *It = TaskRing->Next;
    do {
        if (It->Pid == Pid) {
            Found = It;
            break;
        }
        It = It->Next;
    } while (It != TaskRing->Next);
    
    if (!Found) {
        RestoreFlags(Flags);
        return -1;
    }
    
    // Cannot kill INIT (PID 0)
    if (Found == &InitTask) {
        RestoreFlags(Flags);
        return -1;
    }
    
    // If this is a current task, mark it as a zombie
    if (Found == CurrentTask) {
        Found->State = TASK_ZOMBIE;
        AddToZombieList(Found);
        UnlinkFromRing(Found);
        RestoreFlags(Flags);
        
        // Infinite loop for scheduler to switch
        for (;;) {
            LocalInterruptsEnable();
            Halt();
        }
    }
    
    // Otherwise we just delete it
    UnlinkFromRing(Found);
    RestoreFlags(Flags);
    FreeTaskResources(Found);
    return 0;
}

INT SchedulerKillCurrent(VOID) {
    if (!CurrentTask || CurrentTask == &InitTask) {
        return -1;
    }
    return SchedulerKill(CurrentTask->Pid);
}

BOOL IsAddressInUserSpace(UINT64 Addr) {
    // User space: 0x0000000000400000 - 0x00007FFFFFFFFFFFF
    return (Addr >= USER_CODE_VADDR && Addr < 0x00007FFFFFFFFFFFULL);
}

BOOL IsCurrentFromUser(VOID) {
    if (!CurrentTask) return FALSE;
    return CurrentTask->IsUser;
}
