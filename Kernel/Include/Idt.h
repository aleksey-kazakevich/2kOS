#pragma once

#include <Types.h>

#define IDT_ENTRIES 256

#define TIMER 32
#define KEYBOARD 33
#define SATA_IRQ 34
#define HDA_IRQ  35
#define NVME_IRQ 36
#define MOUSE    37
#define ACPI_SCI 38

#define IDT_GATE_INT 0x8E
#define KERNEL_CODE_SEL  0x08
#define IDT_GATE_SYSCALL 0xEE

struct ATTRIBUTE(packed) IdtEntry {
    UINT16 OffsetLow;
    UINT16 Selector;
    UINT8 Ist;
    UINT8 TypeAttr;
    UINT16 OffsetMid;
    UINT32 OffsetHigh;
    UINT32 Zero;
};

struct ATTRIBUTE(packed) IdtPtr {
    UINT16 Limit;
    UINT64 Base;
};

VOID IdtSetGate(UINT8 Num, VOID (*Handler)(), UINT16 Sel, UINT8 Flags, UINT8 Ist);
VOID IdtInit(VOID);
VOID InitIdt(VOID);
