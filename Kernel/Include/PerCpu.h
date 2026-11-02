#pragma once

#include <Scheduler.h>
#include <SpinLock.h>
#include <Types.h>

#define MAX_CPUS 128

/*
 * Vector for resched IPI.
 * Does not overlap with the registered ones: TIMER=32, KEYBOARD=33, SATA=34, HDA=35, NVME=36, MOUSE=37, ACPI_SCI=38, SATA_IRQ_VECTOR=0x42.
 * We take 0xF1.
 */
#define SCHED_RESCHED_IPI_VECTOR   0xF1

typedef struct PerCpu {
    UINT32 CpuIndex;         /* 0..N-1 */
    UINT32 LapicId;
    UINT32 AcpiUid;
    BOOL   IsBsp;
    BOOL   Online;

    /* GDT/TSS for this CPU */
    VOID  *Gdt;
    VOID  *Tss;
    UINT64 TssRsp0;
    
    UINT64 SyscallStackTop;  // Tss->Rsp0, cache for SyscallEntry
    UINT64 SyscallUserRsp;   // saved in SyscallEntry

    /* Current task (per-CPU scheduler) */
    Task  *CurrentTask;
    Task  *IdleTask;

    /* Compatibility: how many tasks are in the local runqueue */
    volatile UINT32 RunQueueCount;

    /* The stack for interrupts of this CPU */
    VOID  *KernelStack;
    USIZE  KernelStackSize;

    /* Statistics */
    UINT64 Ticks;
    UINT64 IpiCount;

    /* Spinlock for cross-CPU operations */
    SpinLock Lock;

    /* === CFS === */
    UINT64 MinVruntime;         /* min vruntime of ready tasks of this CPU */
    UINT64 LastAccountTick;     /* when was the last time accounted */
    UINT64 SchedFracRemainder;  /* fractional part for delta_vruntime */

    /* === Load balancing === */
    Task   *RunQueue;           /* circular list of completed tasks (tail) */
    SpinLock RunQueueLock;      /* RunQueue, Load, and MinVruntime protection */
    UINT64  Load;               /* the sum of the task weights in RunQueue */
    UINT64  LastBalanceTick;    /* when was the last time a pull-balance */
    volatile BOOL Idle;         /* TBut if the CPU is currently idle */
    volatile BOOL NeedResched;  /* the "reschedule" flag */
} ATTRIBUTE(aligned(64)) PerCpu;

extern PerCpu GPerCpu[MAX_CPUS];
extern UINT32 GCpuCount;

/* Get the current CPU (via %gs) */
STATIC INLINE PerCpu *GetPerCpu(VOID) {
    PerCpu *P;
    asm volatile("mov %%gs:0, %0" : "=r"(P));
    return P;
}

PerCpu *PerCpuGetByLapicId(UINT32 LapicId);
PerCpu *PerCpuGetByIndex(UINT32 Index);

UINT32 PerCpuGetCount(VOID);
UINT32 PerCpuGetOnlineCount(VOID);
PerCpu *PerCpuGetFirst(VOID);

/* Find the least loaded online CPU (for wakeup balancing). */
PerCpu *PerCpuGetLeastLoaded(VOID);