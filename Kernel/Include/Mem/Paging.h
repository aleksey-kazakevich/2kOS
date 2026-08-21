#pragma once

#include <Types.h>
#include <Limine/LimineParse.h>
#include <Asm/Cpu.h>

#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

// ============================================================================
// PTE Flags
// ============================================================================

#define PTE_PRESENT    (1ULL << 0)
#define PTE_WRITABLE   (1ULL << 1)
#define PTE_USER       (1ULL << 2)
#define PTE_WRITE_THROUGH (1ULL << 3)
#define PTE_CACHE_DISABLE (1ULL << 4)
#define PTE_ACCESSED   (1ULL << 5)
#define PTE_DIRTY      (1ULL << 6)
#define PTE_HUGE       (1ULL << 7)
#define PTE_GLOBAL     (1ULL << 8)
#define PTE_NO_EXEC    (1ULL << 63)

// ============================================================================
// Paging levels
// ============================================================================

#define PAGING_LEVEL_4 4
#define PAGING_LEVEL_5 5

// ============================================================================
// 4-level paging (48 bit)
// ============================================================================

#define PML4_SHIFT_4  39
#define PDPT_SHIFT_4  30
#define PD_SHIFT_4    21
#define PT_SHIFT_4    12

#define PML4_INDEX_4(Va) (((UINT64)(Va) >> PML4_SHIFT_4) & 0x1FF)
#define PDPT_INDEX_4(Va) (((UINT64)(Va) >> PDPT_SHIFT_4) & 0x1FF)
#define PD_INDEX_4(Va)   (((UINT64)(Va) >> PD_SHIFT_4) & 0x1FF)
#define PT_INDEX_4(Va)   (((UINT64)(Va) >> PT_SHIFT_4) & 0x1FF)

// ============================================================================
// 5-level paging (57 bits)
// ============================================================================

#define PML5_SHIFT_5  48
#define PML4_SHIFT_5  39
#define PDPT_SHIFT_5  30
#define PD_SHIFT_5    21
#define PT_SHIFT_5    12

#define PML5_INDEX_5(Va) (((UINT64)(Va) >> PML5_SHIFT_5) & 0x1FF)
#define PML4_INDEX_5(Va) (((UINT64)(Va) >> PML4_SHIFT_5) & 0x1FF)
#define PDPT_INDEX_5(Va) (((UINT64)(Va) >> PDPT_SHIFT_5) & 0x1FF)
#define PD_INDEX_5(Va)   (((UINT64)(Va) >> PD_SHIFT_5) & 0x1FF)
#define PT_INDEX_5(Va)   (((UINT64)(Va) >> PT_SHIFT_5) & 0x1FF)

// ============================================================================
// Masks
// ============================================================================

#define PAGE_MASK      0x000FFFFFFFFFF000ULL
#define HUGE_PAGE_MASK 0x000FFFFFFFE00000ULL



#define USER_CODE_VADDR   0x0000000000400000ULL
#define USER_STACK_TOP    0x00007FFFFFF000ULL
#define USER_STACK_DEFAULT (16ULL * PAGE_SIZE)

#define PAGE_ALIGN_UP(N)  (((USIZE)(N) + PAGE_SIZE - 1) & ~(USIZE)(PAGE_SIZE - 1))

// ============================================================================
// Paging state struct
// ============================================================================

typedef struct {
    UINT8 Level;           // 4 or 5
    UINT64 *RootTable;     // PML4 or PML5
    BOOL Initialized;
} PagingState;

// ============================================================================
// Functions to check support
// ============================================================================

static inline BOOL CpuSupports5LevelPaging(VOID) {
    UINT32 Eax, Ebx, Ecx, Edx;
    
    // Checking max CPUID level
    Cpuid(0, &Eax, &Ebx, &Ecx, &Edx);
    if (Eax < 7) return FALSE;
    
    // Leaf 7, Subleaf 0
    Cpuid(7, &Eax, &Ebx, &Ecx, &Edx);
    
    // 5-level paging: bit 16 in ECX
    return (Ecx & (1 << 16)) != 0;
}

static inline BOOL CpuSupports4LevelPaging(VOID) {
    // All x86_64 processors support 4-level paging
    return TRUE;
}

// ============================================================================
// Address translation functions (unchanged)
// ============================================================================

static inline UINT64 PhysToVirt(UINT64 Phys) {
    UINT64 HhdmOffset = LimineGetHHDMOffset();
    return Phys + HhdmOffset;
}

static inline UINT64 VirtToPhys(UINT64 Virt) {
    UINT64 HhdmOffset = LimineGetHHDMOffset();
    return Virt - HhdmOffset;
}

static inline VOID* PhysToVirtPtr(VOID *Phys) {
    return (VOID*)PhysToVirt((UINT64)Phys);
}

static inline VOID* VirtToPhysPtr(VOID *Virt) {
    return (VOID*)VirtToPhys((UINT64)Virt);
}

// ============================================================================
// Public functions
// ============================================================================

VOID PagingInit(VOID);
UINT64 *PagingGetKernelRoot(VOID);
UINT8 PagingGetLevel(VOID);
VOID PagingSwitch(UINT64 *RootTable);

UINT64 PagingLookupVirt(UINT64 *RootTable, UINT64 VirtAddr);
VOID* PagingUserVirtToPtr(UINT64 *RootTable, UINT64 VirtAddr);

INT PagingMapPage(UINT64 *RootTable, UINT64 VirtAddr, UINT64 PhysAddr, UINT64 Flags);
INT PagingMapRange(UINT64 *RootTable, UINT64 VirtStart, UINT64 PhysStart, USIZE Size, UINT64 Flags);
VOID PagingUnmapPage(UINT64 *RootTable, UINT64 VirtAddr);
VOID PagingUnmapRange(UINT64 *RootTable, UINT64 VirtStart, USIZE Size);

UINT64 *PagingCreateUserAddressSpace(VOID);
VOID PagingDestroyUserAddressSpace(UINT64 *RootTable);
INT PagingMapAnonUser(UINT64 *RootTable, UINT64 VirtAddr, USIZE Size, UINT64 Flags);
INT PagingCopyToUser(UINT64 *RootTable, UINT64 Dest, const VOID *Src, USIZE Size);

INT PagingMapUserRegion(UINT64 *RootTable, VOID *Addr, USIZE Size);
VOID PagingUnmapUserRegion(UINT64 *RootTable, VOID *Addr, USIZE Size);

// Auxiliary functions for working with the level
static inline BOOL PagingIs5Level(VOID) {
    return PagingGetLevel() == PAGING_LEVEL_5;
}

static inline BOOL PagingIs4Level(VOID) {
    return PagingGetLevel() == PAGING_LEVEL_4;
}
