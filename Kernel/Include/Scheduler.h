#pragma once

#include <Types.h>
#include <List.h>

#define USER_CS   0x1B
#define USER_SS   0x23
#define KERNEL_CS 0x08
#define KERNEL_SS 0x10

#define KSTACK_SIZE (16 * 1024)

#define X86_EFLAGS_IF     (1ULL << 9)
#define X86_EFLAGS_FIXED  (1ULL << 1)

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
    int Pid;
    TaskState State;
    TaskFrame *Regs;
    VOID *KernelStack;
    USIZE KernelStackSize;
    int ExitCode;
    struct Task *Next;
    struct Task *ZombieNext;
    CHAR Name[32];
    UINT64 *PageTable;
    BOOL IsUser;
} Task;

typedef struct {
    int Pid;
    TaskState State;
    CHAR Name[32];
} TaskInfo;

VOID SchedulerInit(VOID);

INT TaskCreate(VOID (*Entry)(VOID), USIZE StackSize, const CHAR *Name);

INT UserTaskCreate(const VOID *Image,
                   USIZE ImageSize,
                   USIZE UserStackSize,
                   USIZE KernelStackSize,
                   INT ArgC,
                   UINTPTR ArgvUserPtr,
                   const CHAR *Name);

INT TaskList(TaskInfo *Buf, USIZE Max);
INT TaskStop(int Pid);
VOID TaskExit(int ExitCode);
VOID ReapZombies(VOID);
VOID ScheduleFromIsr(UINT64 *Regs, UINT64 **OutRegsPtr);
Task *GetCurrentTask(VOID);
BOOL TaskIsAlive(int Pid);

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
