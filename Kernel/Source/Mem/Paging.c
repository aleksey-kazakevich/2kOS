#include <Mem/Paging.h>
#include <Mem/PhysAlloc.h>
#include <Lib/String.h>
#include <Return.h>
#include <CpuFeatures.h>
#include <Asm/Cpu.h>
#include <Basecon.h>

// ============================================================================
// Paging status
// ============================================================================

static PagingState GPagingState = {
    .Level = PAGING_LEVEL_4,
    .RootTable = NULLPTR,
    .Initialized = FALSE
};

// ============================================================================
// Helper functions for working with tables
// ============================================================================

static UINT64 *AllocTablePage(UINT64 *PhysOut) {
    VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
    if (!Phys)
        return NULLPTR;

    UINT64 PhysAddr = (UINT64)Phys & PAGE_MASK;
    UINT64 *Virt = (UINT64 *)PhysToVirt(PhysAddr);
    MemSet(Virt, 0, PAGE_SIZE);
    if (PhysOut)
        *PhysOut = PhysAddr;
    return Virt;
}

static UINT64* GetNextLevel(UINT64 *Table, UINT64 Index, UINT64 Flags, BOOL Allocate) {
    UINT64 Entry = Table[Index];
    UINT64 Walk = (Flags & (PTE_PRESENT | PTE_WRITABLE | PTE_USER)) | PTE_PRESENT | PTE_WRITABLE;

    if (Entry & PTE_HUGE) {
        if (!Allocate)
            return NULLPTR;

        UINT64 HugePhys = Entry & HUGE_PAGE_MASK;
        UINT64 PtFlags = (Entry & ~HUGE_PAGE_MASK & ~PTE_HUGE) | PTE_PRESENT | PTE_WRITABLE;
        PtFlags |= (Entry & (PTE_USER | PTE_NO_EXEC));

        UINT64 PtPhys = 0;
        UINT64 *Pt = AllocTablePage(&PtPhys);
        if (!Pt)
            return NULLPTR;

        for (INT I = 0; I < 512; I++) {
            UINT64 PagePhys = HugePhys + (UINT64)I * PAGE_SIZE;
            Pt[I] = PagePhys | PtFlags;
        }

        Table[Index] = (PtPhys & PAGE_MASK) | (PtFlags & ~PTE_HUGE);
        return Pt;
    }

    if (!(Entry & PTE_PRESENT)) {
        if (!Allocate)
            return NULLPTR;

        UINT64 Phys = 0;
        UINT64 *Virt = AllocTablePage(&Phys);
        if (!Virt)
            return NULLPTR;

        Table[Index] = (Phys & PAGE_MASK) | Walk;
        return Virt;
    }

    if ((Walk & PTE_USER) && !(Entry & PTE_USER))
        Table[Index] = Entry | PTE_USER;

    return (UINT64 *)PhysToVirtPtr((VOID *)(Entry & PAGE_MASK));
}

// ============================================================================
// Initializing paging (4-level only)
// ============================================================================

VOID PagingInit(VOID) {
    if (GPagingState.Initialized) return;
    
    // We get the current CR3
    UINT64 CR3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(CR3));
    UINT64 *CurrentRoot = (UINT64*)PhysToVirtPtr((VOID*)(CR3 & PAGE_MASK));
    
    // We always use 4-level paging
    GPagingState.Level = PAGING_LEVEL_4;
    GPagingState.RootTable = CurrentRoot;
    
    // Make sure LA57 is turned off
    UINT64 Cr4 = ReadCr4();
    if (Cr4 & (1ULL << 12)) {
        Cr4 &= ~(1ULL << 12);
        WriteCr4(Cr4);
    }
    
    // Enable CPU features
    CpuEnableSmepSmap();
    CpuEnableUmip();
    if (CpuHasAvx()) CpuEnableXsave();
    
    GPagingState.Initialized = TRUE;
}

// ============================================================================
// Getters
// ============================================================================

UINT64 *PagingGetKernelRoot(VOID) {
    return GPagingState.RootTable;
}

UINT8 PagingGetLevel(VOID) {
    return GPagingState.Level;
}

// ============================================================================
// Switching tables
// ============================================================================

VOID PagingSwitch(UINT64 *RootTable) {
    if (!RootTable)
        RootTable = PagingGetKernelRoot();
    if (!RootTable)
        return;

    UINT64 WantPhys = (UINT64)VirtToPhysPtr(RootTable) & PAGE_MASK;
    UINT64 CurPhys = ReadCr3() & PAGE_MASK;
    if (WantPhys == CurPhys)
        return;

    WriteCr3(WantPhys);
}

// ============================================================================
// Finding a physical address
// ============================================================================

UINT64 PagingLookupVirt(UINT64 *RootTable, UINT64 VirtAddr) {
    UINT64 *Pdpt;
    UINT64 *Pdir;
    UINT64 *Ptbl;
    UINT64 Entry;

    if (!RootTable) {
        return 0;
    }

    Entry = RootTable[PML4_INDEX_4(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }

    Pdpt = (UINT64*)PhysToVirtPtr((VOID*)(Entry & PAGE_MASK));
    Entry = Pdpt[PDPT_INDEX_4(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }
    if (Entry & PTE_HUGE) {
        return (Entry & HUGE_PAGE_MASK) + (VirtAddr & (PAGE_SIZE * 512 - 1));
    }

    Pdir = (UINT64*)PhysToVirtPtr((VOID*)(Entry & PAGE_MASK));
    Entry = Pdir[PD_INDEX_4(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }
    if (Entry & PTE_HUGE) {
        return (Entry & HUGE_PAGE_MASK) + (VirtAddr & (PAGE_SIZE * 512 - 1));
    }

    Ptbl = (UINT64*)PhysToVirtPtr((VOID*)(Entry & PAGE_MASK));
    Entry = Ptbl[PT_INDEX_4(VirtAddr)];
    if (!(Entry & PTE_PRESENT)) {
        return 0;
    }

    return (Entry & PAGE_MASK) | (VirtAddr & (PAGE_SIZE - 1));
}

// ============================================================================
// Page mapping
// ============================================================================

INT PagingMapPage(UINT64 *RootTable, UINT64 VirtAddr, UINT64 PhysAddr, UINT64 Flags) {
    if (!RootTable) RETURN(NO_OBJECT);

    UINT64 WalkFlags = PTE_PRESENT | PTE_WRITABLE;
    if (Flags & PTE_USER)
        WalkFlags |= PTE_USER;

    UINT64 *Pdpt = GetNextLevel(RootTable, PML4_INDEX_4(VirtAddr), WalkFlags, TRUE);
    if (!Pdpt) RETURN(NO_OBJECT);

    UINT64 *Pdir = GetNextLevel(Pdpt, PDPT_INDEX_4(VirtAddr), WalkFlags, TRUE);
    if (!Pdir) RETURN(NO_OBJECT);

    UINT64 *Ptbl = GetNextLevel(Pdir, PD_INDEX_4(VirtAddr), WalkFlags, TRUE);
    if (!Ptbl) RETURN(NO_OBJECT);

    UINT64 PteIndex = PT_INDEX_4(VirtAddr);
    Ptbl[PteIndex] = (PhysAddr & PAGE_MASK) | Flags | PTE_PRESENT;

    InvalidateTLBPage(VirtAddr);

    RETURN(SUCCESS);
}

UINT64 *PagingCreateUserAddressSpace(VOID) {
    UINT64 *KernelRoot = PagingGetKernelRoot();
    if (!KernelRoot)
        return NULLPTR;

    UINT64 *UserRootPhys = (UINT64 *)PhysAllocAllocatePage(PhysAllocGet());
    if (!UserRootPhys)
        return NULLPTR;

    UINT64 *UserRoot = (UINT64 *)PhysToVirtPtr(UserRootPhys);
    MemSet(UserRoot, 0, PAGE_SIZE);

    for (INT I = 256; I < 512; I++)
        UserRoot[I] = KernelRoot[I] & ~PTE_USER;

    return UserRoot;
}

INT PagingMapAnonUser(UINT64 *RootTable, UINT64 VirtAddr, USIZE Size, UINT64 Flags) {
    if (!RootTable || Size == 0)
        RETURN(INCORRECT_VALUE);
    if (VirtAddr & (PAGE_SIZE - 1))
        RETURN(INCORRECT_VALUE);

    Flags |= PTE_USER | PTE_PRESENT;
    USIZE Mapped = 0;

    while (Mapped < Size) {
        VOID *PagePhys = PhysAllocAllocatePage(PhysAllocGet());
        if (!PagePhys)
            RETURN(NO_MEMORY);

        UINT64 Phys = (UINT64)PagePhys;
        MemSet((VOID *)PhysToVirt(Phys), 0, PAGE_SIZE);

        if (PagingMapPage(RootTable, VirtAddr + Mapped, Phys, Flags) != SUCCESS) {
            PhysAllocFreePage(PhysAllocGet(), PagePhys);
            RETURN(NO_MEMORY);
        }

        Mapped += PAGE_SIZE;
    }

    RETURN(SUCCESS);
}

INT PagingCopyToUser(UINT64 *RootTable, UINT64 Dest, const VOID *Src, USIZE Size) {
    if (!RootTable || !Src || Size == 0)
        RETURN(INCORRECT_VALUE);

    const UINT8 *S = (const UINT8 *)Src;

    while (Size) {
        UINT64 Phys = PagingLookupVirt(RootTable, Dest);
        if (!Phys)
            RETURN(NO_OBJECT);

        UINT64 Off = Dest & (PAGE_SIZE - 1);
        USIZE Chunk = PAGE_SIZE - Off;
        if (Chunk > Size)
            Chunk = Size;

        MemCpy((VOID *)PhysToVirt(Phys), (const VOID *)S, Chunk);
        Dest += Chunk;
        S += Chunk;
        Size -= Chunk;
    }

    RETURN(SUCCESS);
}

VOID PagingDestroyUserAddressSpace(UINT64 *RootTable) {
    if (!RootTable) return;

    // We release only the user part (slots 0-255)
    for (INT I = 0; I < 256; I++) {
        if (RootTable[I] & PTE_PRESENT) {
            UINT64 *Pdpt = (UINT64*)PhysToVirtPtr((VOID*)(RootTable[I] & PAGE_MASK));
            
            for (INT J = 0; J < 512; J++) {
                if (Pdpt[J] & PTE_PRESENT) {
                    UINT64 *Pdir = (UINT64*)PhysToVirtPtr((VOID*)(Pdpt[J] & PAGE_MASK));
                    
                    for (INT K = 0; K < 512; K++) {
                        if (Pdir[K] & PTE_PRESENT && !(Pdir[K] & PTE_HUGE)) {
                            UINT64 *Ptbl = (UINT64*)PhysToVirtPtr((VOID*)(Pdir[K] & PAGE_MASK));
                            
                            for (INT L = 0; L < 512; L++) {
                                if (Ptbl[L] & PTE_PRESENT) {
                                    UINT64 Phys = Ptbl[L] & PAGE_MASK;
                                    PhysAllocFreePage(PhysAllocGet(), (VOID*)Phys);
                                }
                            }
                            PhysAllocFreePage(PhysAllocGet(), (VOID*)VirtToPhysPtr(Ptbl));
                        } else if (Pdir[K] & PTE_PRESENT && (Pdir[K] & PTE_HUGE)) {
                            UINT64 Phys = Pdir[K] & HUGE_PAGE_MASK;
                            PhysAllocFreePage(PhysAllocGet(), (VOID*)Phys);
                        }
                    }
                    PhysAllocFreePage(PhysAllocGet(), (VOID*)VirtToPhysPtr(Pdir));
                }
            }
            PhysAllocFreePage(PhysAllocGet(), (VOID*)VirtToPhysPtr(Pdpt));
        }
    }
    PhysAllocFreePage(PhysAllocGet(), (VOID*)VirtToPhysPtr(RootTable));
}

// ============================================================================
// Other functions
// ============================================================================

VOID* PagingUserVirtToPtr(UINT64 *RootTable, UINT64 VirtAddr) {
    UINT64 Phys = PagingLookupVirt(RootTable, VirtAddr);
    if (!Phys) {
        return NULLPTR;
    }
    return (VOID*)PhysToVirt(Phys);
}

INT PagingMapRange(UINT64 *RootTable, UINT64 VirtStart, UINT64 PhysStart, USIZE Size, UINT64 Flags) {
    if (!RootTable) RETURN(NO_OBJECT);
    if (Size == 0) RETURN(INCORRECT_VALUE);
    
    UINT64 Virt = VirtStart & ~(PAGE_SIZE - 1);
    UINT64 Phys = PhysStart & ~(PAGE_SIZE - 1);
    UINT64 End = VirtStart + Size;
    
    while (Virt < End) {
        if (PagingMapPage(RootTable, Virt, Phys, Flags) != 0) {
            RETURN(INCORRECT_VALUE);
        }
        Virt += PAGE_SIZE;
        Phys += PAGE_SIZE;
    }
    
    RETURN(SUCCESS);
}

VOID PagingUnmapPage(UINT64 *RootTable, UINT64 VirtAddr) {
    if (!RootTable) return;

    UINT64 *Pdpt = GetNextLevel(RootTable, PML4_INDEX_4(VirtAddr), 0, FALSE);
    if (!Pdpt) return;
    
    UINT64 *Pdir = GetNextLevel(Pdpt, PDPT_INDEX_4(VirtAddr), 0, FALSE);
    if (!Pdir) return;
    
    UINT64 *Ptbl = GetNextLevel(Pdir, PD_INDEX_4(VirtAddr), 0, FALSE);
    if (!Ptbl) return;

    UINT64 PteIndex = PT_INDEX_4(VirtAddr);
    Ptbl[PteIndex] = 0;

    __asm__ volatile("invlpg (%0)" : : "r"(VirtAddr) : "memory");
}

VOID PagingUnmapRange(UINT64 *RootTable, UINT64 VirtStart, USIZE Size) {
    if (!RootTable || Size == 0) return;
    
    UINT64 Virt = VirtStart & ~(PAGE_SIZE - 1);
    UINT64 End = VirtStart + Size;
    
    while (Virt < End) {
        PagingUnmapPage(RootTable, Virt);
        Virt += PAGE_SIZE;
    }
}

INT PagingMapUserRegion(UINT64 *RootTable, VOID *Addr, USIZE Size) {
    UINT64 Virt = (UINT64)Addr;
    UINT64 Flags = PTE_USER | PTE_WRITABLE | PTE_NO_EXEC;
    
    return PagingMapRange(RootTable, Virt, Virt, Size, Flags);
}

VOID PagingUnmapUserRegion(UINT64 *RootTable, VOID *Addr, USIZE Size) {
    PagingUnmapRange(RootTable, (UINT64)Addr, Size);
}