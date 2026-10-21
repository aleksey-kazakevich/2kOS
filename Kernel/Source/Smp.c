#include <Drivers/Irq/Apic/ApicRegs.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Limine/LimineParse.h>
#include <Drivers/Cpu/Cpu.h>
#include <Limine/limine.h>
#include <Mem/PhysAlloc.h>
#include <Mem/Paging.h>
#include <Time/Timer.h>
#include <Lib/String.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <Asm/Cpu.h>
#include <PerCpu.h>
#include <Return.h>
#include <Smp.h>
#include <Idt.h>
#include <Tss.h>

#define IA32_GS_BASE_MSR   0xC0000101
#define AP_STACK_SIZE      (16 * 1024)

STATIC volatile UINT32 GApStarted = 0;
STATIC volatile BOOL   GSmpReady   = FALSE;

extern struct IdtPtr Idtp;
EXTERN(Timer, GTimer);

/* ============================================================================
 * RESCHED IPI
 * ============================================================================ */

VOID SchedReschedIpiHandler(VOID) {
    PerCpu *Self = GetPerCpu();
    if (Self) {
        Self->NeedResched = TRUE;
        Self->IpiCount++;
    }
    ApicEoi();
}

VOID ApEntry(struct limine_mp_info *Info) {
    PerCpu *Self = PerCpuGetByLapicId(Info->lapic_id);
    if (!Self) {
        for (;;) Halt();
    }

    /* 1. GDT/GS. */
    WriteMSR(IA32_GS_BASE_MSR, (UINT64)(UINTPTR)Self);
    TssInitPerCpu(Self);

    /* 2. IDT. */
    LoadIDT(&Idtp);

    /* 3. The scheduler is up to the timer. Creates an IdleTask. */
    SchedulerInitPerCpu(Self->CpuIndex);

    /* 4. APIC + timer. TimerIsr is now safe. */
    ApicEnable();
    ApicWriteReg(LAPIC_TIMER_DIV, TIMER_DIV_16);
    ApicWriteReg(LAPIC_LVT_TIMER, TIMER | TIMER_PERIODIC);
    ApicWriteReg(LAPIC_TIMER_INITCNT, GTimer.InitCount);

    /* 5. Online, counter, print, IF=1. */
    Self->Online = TRUE;
    __sync_fetch_and_add(&GApStarted, 1);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "smp: cpu %u (apic %u) online\n",
                  Self->CpuIndex, Self->LapicId);

    LocalInterruptsEnable();

    for (;;) Halt();
}

/* ============================================================================
 * SMP INIT
 * ============================================================================ */

INT SmpInit(VOID) {
    /* Fallback: No Limine MP */
    if (!LimineMpAvailable()) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "smp: limine mp not available, uniprocessor mode\n");

        MemSet(&GPerCpu[0], 0, sizeof(PerCpu));
        GPerCpu[0].CpuIndex = 0;
        GPerCpu[0].LapicId  = ApicGetId();
        GPerCpu[0].IsBsp    = TRUE;
        GPerCpu[0].Online   = TRUE;
        SpinLockInit(&GPerCpu[0].Lock);
        GCpuCount = 1;

        WriteMSR(IA32_GS_BASE_MSR, (UINT64)(UINTPTR)&GPerCpu[0]);

        TssInitPerCpu(&GPerCpu[0]);

        /* IPI Vector registration */
        IdtSetGate(SCHED_RESCHED_IPI_VECTOR,
                   (VOID(*)())SchedReschedIpiHandler,
                   KERNEL_CODE_SEL, IDT_GATE_INT, 0);

        SchedulerInitPerCpu(0);

        GSmpReady = TRUE;
        RETURN(SUCCESS);
    }

    UINT64 Count = LimineMpCpuCount();
    UINT32 BspLapicId = LimineMpBspLapicId();

    if (Count == 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "smp: no cpus reported\n");
        RETURN(NO_OBJECT);
    }
    if (Count > MAX_CPUS) Count = MAX_CPUS;

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "smp: %llu cpu(s), bsp apic id %u\n",
                  Count, BspLapicId);

    MemSet(GPerCpu, 0, sizeof(GPerCpu));
    for (UINT64 I = 0; I < Count; I++) {
        struct limine_mp_info *Info = LimineMpCpu(I);
        if (!Info) continue;

        PerCpu *P = &GPerCpu[I];
        P->CpuIndex = (UINT32)I;
        P->LapicId  = Info->lapic_id;
        P->AcpiUid  = Info->processor_id;
        P->IsBsp    = (Info->lapic_id == BspLapicId);
        P->Online   = FALSE;
        SpinLockInit(&P->Lock);
    }
    GCpuCount = (UINT32)Count;

    PerCpu *Bsp = NULLPTR;
    for (UINT32 I = 0; I < GCpuCount; I++) {
        if (GPerCpu[I].IsBsp) { Bsp = &GPerCpu[I]; break; }
    }
    if (!Bsp) { Bsp = &GPerCpu[0]; Bsp->IsBsp = TRUE; }

    Bsp->Online = TRUE;
    WriteMSR(IA32_GS_BASE_MSR, (UINT64)(UINTPTR)Bsp);
    TssInitPerCpu(Bsp); 

    /* IPI vector registration (on BSP; AP will be inherited via idtp). */
    IdtSetGate(SCHED_RESCHED_IPI_VECTOR,
               (VOID(*)())SchedReschedIpiHandler,
               KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    /* Initializing the scheduler for BSP: InitTask in PRQ, IdleTask is there. */
    SchedulerInitPerCpu(Bsp->CpuIndex);

    /* Launching an AP. */
    for (UINT32 I = 0; I < GCpuCount; I++) {
    	if (GPerCpu[I].IsBsp) continue;
    	struct limine_mp_info *Info = LimineMpCpu(I);
    	if (!Info) continue;
    	__atomic_store_n(&Info->goto_address, ApEntry, __ATOMIC_SEQ_CST);
    	// We are waiting for AP to start
    	UINT64 Start = TimerTicks();
    	while (!GPerCpu[I].Online) {
            if ((TimerTicks() - Start) > TimerFreq() * 2) break;
            CpuPause();
    	}
    }

    /* We are waiting for the AP to start (5 sec timeout). */
    if (GCpuCount > 1) {
        UINT64 Start = TimerTicks();
        UINT64 Freq = TimerFreq();
        if (Freq == 0) Freq = 1000;
        UINT64 TimeoutTicks = Freq * 5;

        while (GApStarted < GCpuCount - 1) {
            if ((TimerTicks() - Start) > TimeoutTicks) {
                BaseconPrintf(BASECON_TYPE_WARNING,
                              "smp: timeout (%u/%u AP started)\n",
                              GApStarted, GCpuCount - 1);
                break;
            }
            CpuPause();
        }
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "smp: %u/%u cpu(s) online\n",
                  GApStarted + 1, GCpuCount);

    GSmpReady = TRUE;
    RETURN(SUCCESS);
}

BOOL SmpIsReady(VOID) { return GSmpReady; }

UINT32 SmpGetOnlineCount(VOID) { return GApStarted + 1; }