#include <Asm/Cpu.h>
#include <Lib/String.h>
#include <Types.h>

static UINT8 DfStack[4096] ATTRIBUTE(aligned(16));

struct GdtPtr {
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
_Static_assert(OffsetOf(Tss64, Rsp0) == 4, "TSS.RSP0 must be at offset 4");
_Static_assert(OffsetOf(Tss64, Ist1) == 36, "TSS.IST1 must be at offset 36");

extern UINT8 TssBuffer[104];
extern UINT64 Gdt[];
extern struct GdtPtr GdtDesc;

UINT64 SyscallStackTop;
UINT64 SyscallUserRsp;

static VOID GdtSetTss(UINT64 Base, UINT32 Limit) {
    UINT64 Low;
    UINT64 High;

    Low = ((UINT64)Limit & 0xFFFF) |
          ((Base & 0xFFFFFFULL) << 16) |
          (0x89ULL << 40) |
          (((UINT64)Limit & 0xF0000ULL) << 32) |
          ((Base & 0xFF000000ULL) << 32);

    High = Base >> 32;

    Gdt[5] = Low;
    Gdt[6] = High;
}

VOID TssInit(VOID) {
    Tss64 *Tss = (Tss64 *)&TssBuffer;
    extern UINT8 StackEnd[];

    MemSet(Tss, 0, sizeof(Tss64));
    Tss->IoMapBase = sizeof(Tss64);
    Tss->Rsp0 = (UINT64)(UINTPTR)StackEnd;
    SyscallStackTop = Tss->Rsp0;
    Tss->Ist1 = (UINT64)(UINTPTR)DfStack + sizeof(DfStack);

    GdtSetTss((UINT64)(UINTPTR)Tss, sizeof(Tss64) - 1);
    LoadGDT((VOID *)&GdtDesc);
    asm volatile("ltr %w0" : : "r"((UINT16)0x28));
}

VOID TssSetRsp0(UINT64 Rsp0) {
    Tss64 *Tss = (Tss64 *)&TssBuffer;
    Tss->Rsp0 = Rsp0;
    SyscallStackTop = Rsp0;
}
