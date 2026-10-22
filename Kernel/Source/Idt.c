#include <Drivers/Irq/Apic/Apic.h>
#include <InterruptList.h>
#include <Asm/Cpu.h>
#include <Basecon.h>
#include <Asm/Io.h>
#include <Types.h>
#include <Idt.h>

/* ============================================================================
 * MSR addresses for SYSCALL/SYSRET
 * ============================================================================ */

#define IA32_EFER          0xC0000080
#define IA32_STAR          0xC0000081
#define IA32_LSTAR         0xC0000082
#define IA32_CSTAR         0xC0000083
#define IA32_SFMASK        0xC0000084

#define EFER_SCE           (1ULL << 0)  /* SYSCALL Enable */

#define SYSCALL_CS         0x08
#define SYSCALL_USER_CS    0x18   /* base; RPL will be added at SYSRET */

/* ============================================================================
 * SYSCALL/SYSRET setup
 * ============================================================================ */

extern VOID SyscallEntry(VOID);

VOID SetupSyscall(VOID) {
    /* 1. Enabling SYSCALL/SYSRET via EFER.SCE */
    UINT64 Efer = ReadMSR(IA32_EFER);
    Efer |= EFER_SCE;
    WriteMSR(IA32_EFER, Efer);

    /* 2.Setting up selectors in STAR */
    UINT64 Star = 0;
    Star |= (UINT64)(SYSCALL_USER_CS & 0xFFFF) << 48;
    Star |= (UINT64)(SYSCALL_CS      & 0xFFFF) << 32;
    WriteMSR(IA32_STAR, Star);

    /* 3. Specifying the address of the SYSCALL handler */
    WriteMSR(IA32_LSTAR, (UINT64)SyscallEntry);
    WriteMSR(IA32_SFMASK, 0x0000000000004700ULL);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "idt: syscall configured\n");
}

struct IdtEntry Idt[IDT_ENTRIES];
struct IdtPtr  Idtp;

VOID IdtSetGate(UINT8 Num, VOID (*Handler)(), UINT16 Sel, UINT8 Flags, UINT8 Ist) {
    if (Num >= IDT_ENTRIES)
        return;
    if (!Handler)
        return;   /* protection against accidental NULL — the vector remains as it was */

    UINT64 Base = (UINT64)Handler;

    Idt[Num].OffsetLow  = (UINT16)(Base & 0xFFFF);
    Idt[Num].Selector   = Sel;
    Idt[Num].Ist        = (UINT8)(Ist & 0x07);
    Idt[Num].TypeAttr   = Flags;
    Idt[Num].OffsetMid  = (UINT16)((Base >> 16) & 0xFFFF);
    Idt[Num].OffsetHigh = (UINT32)((Base >> 32) & 0xFFFFFFFF);
    Idt[Num].Zero       = 0;
}

/* Vectors for which the CPU automatically pushes error code.
 * See Intel SDM Vol 3, Table 6-1. */
STATIC BOOL IdtVectorHasErrorCode(UINT32 Vector) {
    switch (Vector) {
        case 8:   /* #DF  — Double Fault */
        case 10:  /* #TS  — Invalid TSS */
        case 11:  /* #NP  — Segment Not Present */
        case 12:  /* #SS  — Stack Segment Fault */
        case 13:  /* #GP  — General Protection */
        case 14:  /* #PF  — Page Fault */
        case 17:  /* #AC  — Alignment Check */
        case 21:  /* #CP  — Control Protection */
        case 29:  /* #VC  — VMM Communication */
        case 30:  /* #SX  — Security Exception */
            return TRUE;
        default:
            return FALSE;
    }
}

STATIC VOID IdtSanityCheck(VOID) {
    for (UINT32 I = 0; I < IDT_ENTRIES; I++) {
        UINT64 Base = ((UINT64)Idt[I].OffsetHigh << 32)
                    | ((UINT64)Idt[I].OffsetMid  << 16)
                    | ((UINT64)Idt[I].OffsetLow);
    }
}

VOID IdtInit(VOID) {
    Idtp.Limit = (UINT16)(sizeof(Idt) - 1);
    Idtp.Base  = (UINT64)&Idt;
    for (UINT32 I = 0; I < IDT_ENTRIES; I++) {
        if (IdtVectorHasErrorCode(I)) {
            IdtSetGate((UINT8)I, IsrStubGenericError,
                       KERNEL_CODE_SEL, IDT_GATE_INT, 0);
        } else {
            IdtSetGate((UINT8)I, IsrStubGeneric,
                       KERNEL_CODE_SEL, IDT_GATE_INT, 0);
        }
    }

    IdtSetGate(0,  DivideErrorStub,        KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(1,  DebugExceptionStub,     KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(2,  NmiStub,                KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(3,  BreakpointStub,         KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(4,  OverflowStub,           KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(5,  BoundStub,              KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(6,  InvalidOpcodeStub,      KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(7,  DeviceNotAvailableStub, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(8,  DoubleFaultStub,        KERNEL_CODE_SEL, IDT_GATE_INT, 1);
    /* 9  — Coprocessor Segment Overrun (obsolete) — we leave generic */
    IdtSetGate(10, InvalidTssStub,         KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(11, SegmentNotPresentStub,  KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(12, StackSegmentStub,       KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(13, GeneralProtectionStub,  KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(14, PageFaultStub,          KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    /* 15 — Reserved */
    IdtSetGate(16, FpuErrorStub,           KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(17, IsrStubGenericError,    KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(18, MachineCheckStub,       KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    IdtSetGate(19, SimdErrorStub,          KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    IdtSetGate(32, TimerIsr, KERNEL_CODE_SEL, IDT_GATE_INT, 0);

    /* We check that all 256 gates are filled. */
    IdtSanityCheck();

    LoadIDT(&Idtp);
}

VOID InitIdt(VOID) {
    IdtInit();
    SetupSyscall();
}