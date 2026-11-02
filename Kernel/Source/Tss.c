#include <Lib/String.h>
#include <Asm/Cpu.h>
#include <PerCpu.h>
#include <Types.h>
#include <Tss.h>

#define MAX_TSS  MAX_CPUS

struct ATTRIBUTE(packed) GdtPtr {
    UINT16 Limit;
    UINT64 Base;
};

typedef struct {
    UINT32 Reserved0;
    UINT64 Rsp0;
    UINT64 Rsp1;
    UINT64 Rsp2;
    UINT64 Reserved1;
    UINT64 Ist1;
    UINT64 Ist2;
    UINT64 Ist3;
    UINT64 Ist4;
    UINT64 Ist5;
    UINT64 Ist6;
    UINT64 Ist7;
    UINT64 Reserved2;
    UINT16 Reserved3;
    UINT16 IoMapBase;
} ATTRIBUTE(packed) Tss64;

_Static_assert(sizeof(Tss64) == 104, "64-bit TSS must be 104 bytes");

// === PER-CPU TSS AND DF-STACKS ===
STATIC Tss64 GTss[MAX_TSS] ATTRIBUTE(aligned(16));
STATIC UINT8 GDfStack[MAX_TSS][4096] ATTRIBUTE(aligned(16));

// === PER-CPU GDT ===
// GDT: 0=null, 1=kcode, 2=kdata, 3=ucode, 4=udata, 5-6=TSS (low,high)
STATIC UINT64 GGdt[MAX_TSS][7] ATTRIBUTE(aligned(16));

extern UINT8 StackEnd[];

STATIC VOID GdtSetTssInGdt(UINT64 *Gdt, UINT64 Base, UINT32 Limit) {
    UINT64 Low = ((UINT64)Limit & 0xFFFF) |
                 ((Base & 0xFFFFFFULL) << 16) |
                 (0x89ULL << 40) |
                 (((UINT64)Limit & 0xF0000ULL) << 32) |
                 ((Base & 0xFF000000ULL) << 32);
    UINT64 High = Base >> 32;
    Gdt[5] = Low;
    Gdt[6] = High;
}

STATIC VOID GdtInitBase(UINT64 *Gdt) {
    Gdt[0] = 0x0000000000000000ULL;   // null
    Gdt[1] = 0x00209A0000000000ULL;   // kernel code
    Gdt[2] = 0x0000920000000000ULL;   // kernel data
    Gdt[3] = 0x0020FA0000000000ULL;   // user code
    Gdt[4] = 0x0000F20000000000ULL;   // user data
    Gdt[5] = 0;                        // TSS low
    Gdt[6] = 0;                        // TSS high
}

VOID TssInitPerCpu(PerCpu *P) {
    if (!P) return;
    UINT32 Idx = P->CpuIndex;
    if (Idx >= MAX_TSS) return;

    Tss64 *Tss = &GTss[Idx];
    MemSet(Tss, 0, sizeof(Tss64));
    Tss->IoMapBase = sizeof(Tss64);

    // Rsp0: for BSP — top of the initial kernel stack; for AP — temporarily top
    // DF stack, then SchedulerInitPerCpu will overwrite via TssSetRsp0.
    if (P->IsBsp) {
        Tss->Rsp0 = (UINT64)(UINTPTR)StackEnd;
    } else {
        Tss->Rsp0 = (UINT64)(UINTPTR)&GDfStack[Idx][sizeof(GDfStack[0])];
    }
    Tss->Ist1 = (UINT64)(UINTPTR)&GDfStack[Idx][sizeof(GDfStack[0])];

    P->Tss      = Tss;
    P->TssRsp0  = Tss->Rsp0;
    P->SyscallStackTop = Tss->Rsp0;
    P->SyscallUserRsp  = 0;

    // GDT for this CPU
    UINT64 *Gdt = GGdt[Idx];
    GdtInitBase(Gdt);
    GdtSetTssInGdt(Gdt, (UINT64)(UINTPTR)Tss, sizeof(Tss64) - 1);
    P->Gdt = Gdt;

    struct GdtPtr GdtPtr = {
        .Limit = (UINT16)(sizeof(GGdt[0]) - 1),
        .Base  = (UINT64)(UINTPTR)Gdt
    };
    LoadGDT(&GdtPtr);

    // Far jump so that CS is rebooted to the new GDT
    asm volatile(
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        ::: "rax", "memory"
    );

    asm volatile("ltr %w0" : : "r"((UINT16)0x28));
}

VOID TssSetRsp0(PerCpu *P, UINT64 Rsp0) {
    if (!P || !P->Tss) return;
    Tss64 *Tss = (Tss64 *)P->Tss;
    Tss->Rsp0 = Rsp0;
    P->TssRsp0 = Rsp0;
    P->SyscallStackTop = Rsp0;
}