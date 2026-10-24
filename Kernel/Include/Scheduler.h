#pragma once

#include <Fs/FdTable.h>
#include <KObj/KObj.h>
#include <Fs/Vfs.h>
#include <Types.h>
#include <List.h>

#define USER_CS   0x1B
#define USER_SS   0x23
#define KERNEL_CS 0x08
#define KERNEL_SS 0x10

#define KSTACK_SIZE (16 * 1024)

#define X86_EFLAGS_IF     (1ULL << 9)
#define X86_EFLAGS_FIXED  (1ULL << 1)

/* ============================================================================ CFS-like scheduler: priorities via nice and weights ============================================================================ */

#define NICE_MIN        (-20)
#define NICE_MAX        ( 19)
#define NICE_DEFAULT    (  0)
#define NICE_LEVELS     (NICE_MAX - NICE_MIN + 1)   /* 40 */

#define NICE_0_WEIGHT   (1024ULL)

extern const UINT32 GSchedWeights[NICE_LEVELS];

STATIC INLINE UINT32 NiceToWeight(INT Nice) {
    if (Nice < NICE_MIN) Nice = NICE_MIN;
    if (Nice > NICE_MAX) Nice = NICE_MAX;
    return GSchedWeights[Nice - NICE_MIN];
}

STATIC INLINE INT WeightToNice(UINT32 Weight) {
    for (INT I = 0; I < NICE_LEVELS; I++) {
        if (GSchedWeights[I] == Weight) return I + NICE_MIN;
    }
    for (INT I = NICE_LEVELS - 1; I >= 0; I--) {
        if (GSchedWeights[I] <= Weight) return I + NICE_MIN;
    }
    return NICE_MAX;
}

typedef enum {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_ZOMBIE
} TaskState;

typedef struct TaskFrame {
    UINT64 R15;
    UINT64 R14;
    UINT64 R13;
    UINT64 R12;
    UINT64 R11;
    UINT64 R10;
    UINT64 R9;
    UINT64 R8;
    UINT64 Rdi;
    UINT64 Rsi;
    UINT64 Rbp;
    UINT64 Rbx;
    UINT64 Rdx;
    UINT64 Rcx;
    UINT64 Rax;
    UINT64 Rip;
    UINT64 Cs;
    UINT64 Rflags;
    UINT64 Rsp;
    UINT64 Ss;
} TaskFrame;

typedef struct Task {
    UINT8 FpuState[512] ATTRIBUTE(aligned(16));
    int Pid;
    TaskState State;
    TaskFrame *Regs;
    VOID *KernelStack;
    USIZE KernelStackSize;
    INT ExitCode;

    /* Global ring of all tasks (to bypass / kill / list) */
    struct Task *Next;
    /* Local ring runqueue CPU (for scheduling) */
    struct Task *RunNext;

    struct Task *ZombieNext;
    CHAR Name[32];
    UINT64 *PageTable;
    BOOL IsUser;
    UINT64 BrkStart;
    UINT64 BrkCurrent;
    UINT64 BrkMax;

    /* === CFS === */
    INT      Nice;
    UINT32   Weight;
    UINT64   Vruntime;
    UINT64   ExecTicks;
    UINT64   WakeupTick;
    UINT64   LastRunTick;      /* when was the last time RUNNING */
    BOOL     InRing;           /* physically in the global TaskRing */
    BOOL     OnRunQueue;       /* in per-CPU RunQueue */

    /* === Load balancing === */
    INT32    Cpu;              /* current/last CPU; -1 = migrating */
    UINT32   CpuAffinity;      /* bitmask of valid CPUs; 0 = any */

    KObjHandle KObjHandles[KOBJ_HANDLE_MAX];

    struct KObj *StdoutObj;
    struct KObj *StderrObj;
    struct KObj *StdwarnObj;
    struct KObj *StdinObj;

    FdTable Fds;

    struct VfsInode *Cwd;
} ATTRIBUTE(aligned(16)) Task;

typedef struct {
    INT Pid;
    TaskState State;
    CHAR Name[32];
} TaskInfo;

typedef struct {
    INT      Pid;
    TaskState State;
    CHAR     Name[32];
    INT      Nice;
    UINT32   Weight;
    UINT64   Vruntime;
    UINT64   ExecTicks;
    INT32    Cpu;
} TaskInfoEx;

VOID SchedulerInit(VOID);
VOID SchedulerInitPerCpu(UINT32 CpuIndex);   /* called from SmpInit/ApEntry */

INT TaskCreate(VOID (*Entry)(VOID), USIZE StackSize, const CHAR *Name);

INT UserTaskCreate(const VOID *Image,
                   USIZE ImageSize,
                   USIZE UserStackSize,
                   USIZE KernelStackSize,
                   INT ArgC,
                   UINTPTR ArgvUserPtr,
                   const CHAR *Name);

INT TaskList(TaskInfo *Buf, USIZE Max);
INT TaskListEx(TaskInfoEx *Buf, USIZE Max);
INT TaskStop(int Pid);
VOID TaskExit(int ExitCode);
VOID ReapZombies(VOID);
VOID ScheduleFromIsr(UINT64 *Regs, UINT64 **OutRegsPtr);
Task *GetCurrentTask(VOID);
BOOL TaskIsAlive(int Pid);

/* === CFS: Priority management === */
INT  TaskSetNice(INT Pid, INT Nice);
INT  TaskGetNice(INT Pid);
VOID TaskSetNiceSelf(INT Nice);

TaskFrame *PrepareInitialStack(UINT64 Entry,
                               VOID *KernelStackTop,
                               UINT64 UserOrKernelRsp,
                               INT ArgC,
                               UINTPTR ArgvPtr,
                               BOOL UserMode);

VOID UpdateKernelStack(UINT64 KernelStackTop);
VOID *AllocKernelStack(USIZE Bytes, USIZE *OutSize);

INT SchedulerKill(int Pid);
INT SchedulerKillCurrent(VOID);
BOOL IsAddressInUserSpace(UINT64 Addr);
BOOL IsCurrentFromUser(VOID);

INT UserTaskCreateFromPageTable(UINT64 *Pml4,
                                 UINT64 Entry,
                                 UINT64 UserSp,
                                 UINT64 HeapStart,
                                 const CHAR *Name);

VOID TaskBlock(VOID);
VOID TaskWakeup(Task *T);

/* === SMP: migration and balancing === */

/*
 * Explicit migration of the task to the specified CPU.
 * Returns SUCCESS or error code.
 */
INT SchedMigrateTask(Task *T, INT TargetCpu);

/* Select the CPU for a new/awake task. */
INT SchedPickCpuForTask(Task *T);

/* Periodic balancing is called from the tick. */
VOID SchedLoadBalance(VOID);

/* The idle CPU drags the task from the busiest one. */
BOOL SchedIdleBalance(VOID);

/*
   Install affinity.
   Mask = 0 -> any CPU.
 */
INT SchedSetAffinity(INT Pid, UINT32 Mask);
UINT32 SchedGetAffinity(INT Pid);

/* Diagnostics. */
VOID SchedDumpRunQueues(VOID);

/* === SMP-specific === */

/* Global InitTask (BSP) */
EXTERN(Task, InitTask);

/* Global list of all tasks */
EXTERN(Task, *TaskRing);