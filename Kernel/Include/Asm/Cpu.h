#pragma once

#include <Types.h>

static inline VOID LocalInterruptsDisable(VOID) {
    asm volatile ("cli" : : : "memory");
}

static inline VOID LocalInterruptsEnable(VOID) {
    asm volatile ("sti" : : : "memory");
}

static inline VOID Halt(VOID) {
    asm volatile ("hlt");
}

static inline UINT64 ReadTimeStampCounter(VOID) {
    UINT32 Lo, Hi;
    asm volatile ("rdtsc" : "=a"(Lo), "=d"(Hi));
    return ((UINT64)Hi << 32) | Lo;
}

static inline UINT64 ReadCr0(VOID) {
    UINT64 Val;
    asm volatile("mov %%cr0, %0" : "=r"(Val));
    return Val;
}

static inline VOID WriteCr0(UINT64 Val) {
    asm volatile("mov %0, %%cr0" : : "r"(Val) : "memory");
}

static inline UINT64 ReadCr3(VOID) {
    UINT64 Val;
    asm volatile("mov %%cr3, %0" : "=r"(Val));
    return Val;
}

static inline VOID WriteCr3(UINT64 Val) {
    asm volatile("mov %0, %%cr3" : : "r"(Val) : "memory");
}

static inline UINT64 ReadCr4(VOID) {
    UINT64 Val;
    asm volatile("mov %%cr4, %0" : "=r"(Val));
    return Val;
}

static inline VOID WriteCr4(UINT64 Val) {
    asm volatile("mov %0, %%cr4" : : "r"(Val) : "memory");
}

static inline UINT64 ReadRflags(VOID) {
    UINT64 Flags;
    asm volatile("pushfq; popq %0" : "=g"(Flags));
    return Flags;
}

static inline VOID WriteRflags(UINT64 Flags) {
    asm volatile("pushq %0; popfq" : : "g"(Flags) : "memory", "cc");
}

static inline VOID InvalidateTLBPage(UINT64 Addr) {
    asm volatile("invlpg (%0)" : : "r"(Addr) : "memory");
}

static inline VOID LoadIDT(VOID* Ptr) {
    asm volatile("lidt (%0)" : : "r"(Ptr));
}

static inline VOID LoadGDT(VOID* Ptr) {
    asm volatile("lgdt (%0)" : : "r"(Ptr));
}

static inline UINT64 ReadMSR(UINT32 Msr) {
    UINT32 Lo, Hi;
    asm volatile("rdmsr" : "=a"(Lo), "=d"(Hi) : "c"(Msr));
    return ((UINT64)Hi << 32) | Lo;
}

static inline VOID WriteMSR(UINT32 Msr, UINT64 Val) {
    UINT32 Lo = (UINT32)Val;
    UINT32 Hi = (UINT32)(Val >> 32);
    asm volatile("wrmsr" : : "a"(Lo), "d"(Hi), "c"(Msr));
}

static inline VOID Cpuid(UINT32 Code, UINT32 *Eax, UINT32 *Ebx, UINT32 *Ecx, UINT32 *Edx) {
    asm volatile("cpuid"
                 : "=a"(*Eax), "=b"(*Ebx), "=c"(*Ecx), "=d"(*Edx)
                 : "a"(Code), "c"(0));
}

static inline VOID CpuidLeaf(UINT32 Code, UINT32 Subleaf, UINT32 *Eax, UINT32 *Ebx, UINT32 *Ecx, UINT32 *Edx) {
    asm volatile("cpuid"
                 : "=a"(*Eax), "=b"(*Ebx), "=c"(*Ecx), "=d"(*Edx)
                 : "a"(Code), "c"(Subleaf));
}

static inline VOID CpuPause(VOID) {
    asm volatile("pause");
}

static inline BOOL CheckInterruptStatus(VOID) {
    UINT64 Flags;
    asm volatile("pushfq; popq %0" : "=g"(Flags));
    return (Flags & 0x200) ? TRUE : FALSE;
}

static inline VOID AsmFlushCacheRange(VOID *Addr, UINTN Size) {
    UINT8 *P = (UINT8 *)Addr;
    while (Size > 0) {
        __builtin_ia32_clflush(P);
        P += 64;
        Size -= (Size >= 64) ? 64 : Size;
    }
    __builtin_ia32_sfence();
}

static inline UINTPTR SaveFlags(VOID) {
    UINTPTR Flags;
    asm volatile("pushfq; pop %0" : "=r"(Flags)::"memory");
    return Flags;
}

static inline VOID RestoreFlags(UINTPTR Flags) {
    asm volatile("push %0; popfq" ::"r"(Flags) : "memory", "cc");
}

static inline UINTPTR GetRbp(VOID) {
    UINTPTR Rbp;
    asm volatile("mov %%rbp, %0" : "=r"(Rbp));
    return Rbp;
}

static inline UINTPTR GetRip(VOID) {
    UINTPTR Rip;
    asm volatile("call 1f; 1: pop %0" : "=r"(Rip));
    return Rip;
}
