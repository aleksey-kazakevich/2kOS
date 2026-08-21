#include <CpuFeatures.h>
#include <Return.h>
#include <Asm/Cpu.h>

#define CR4_SMEP    (1 << 20)
#define CR4_SMAP    (1 << 21)
#define CR4_UMIP    (1 << 5)  

/*
 * ============================================================================
 * Helper functions for checking support via CPUID
 * ============================================================================
 */

static BOOL CpuHasSmep(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    // Leaf 7, Subleaf 0 — Extended Features
    Cpuid(7, &Eax, &Ebx, &Ecx, &Edx);
    
    // SMEP bit 7 in EBX
    return (Ebx & (1 << 7)) != 0;
}

static BOOL CpuHasSmap(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    Cpuid(7, &Eax, &Ebx, &Ecx, &Edx);
    
    // SMAP bit 20 in EBX
    return (Ebx & (1 << 20)) != 0;
}

static BOOL CpuHasUmip(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    Cpuid(7, &Eax, &Ebx, &Ecx, &Edx);
    
    // UMIP bit 2 in ECX
    return (Ecx & (1 << 2)) != 0;
}

/*
 * ============================================================================
 * Basic switching functions
 * ============================================================================
 */

INT CpuEnableSmepSmap(VOID) {
    UINT64 Cr4 = ReadCr4();
    BOOL Changed = FALSE;
    
    #ifndef DISABLE_SMEP
    if (CpuHasSmep()) {
        Cr4 |= CR4_SMEP;
        Changed = TRUE;
    }
    #endif
    
    #ifndef DISABLE_SMAP
    if (CpuHasSmap()) {
        Cr4 |= CR4_SMAP;
        Changed = TRUE;
    }
    #endif
    
    if (Changed) {
        WriteCr4(Cr4);
    }
    
    return SUCCESS;
}

INT CpuEnableUmip(VOID) {
    UINT64 Cr4 = ReadCr4();
    
    if (CpuHasUmip()) {
        Cr4 |= CR4_UMIP;
        WriteCr4(Cr4);
    }
    
    return SUCCESS;
}

/*
 * ============================================================================
 * Other functions (unchanged)
 * ============================================================================
 */

BOOL CpuHasAvx(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    Cpuid(1, &Eax, &Ebx, &Ecx, &Edx);
    
    /* AVX bit 28 in ECX*/
    if (!(Ecx & (1 << 28))) return FALSE;
    
    /* Проверка XSAVE (бит 27 в ECX) */
    if (!(Ecx & (1 << 27))) return FALSE;
    
    return TRUE;
}

VOID CpuEnableXsave(VOID) {
    UINT64 Cr4 = ReadCr4();
    Cr4 |= (1 << 18);  /* CR4.OSXSAVE */
    WriteCr4(Cr4);
    
    /* XGETBV: request XCR0*/
    UINT32 Eax, Edx;
    asm volatile("xgetbv" : "=a"(Eax), "=d"(Edx) : "c"(0));
    UINT64 Xcr0 = ((UINT64)Edx << 32) | Eax;
    
    Xcr0 |= 0x07;  /* x87 + SSE + AVX */
    
    /* XSETBV */
    asm volatile("xsetbv" : : "c"(0), "a"((UINT32)Xcr0), "d"((UINT32)(Xcr0 >> 32)));
}