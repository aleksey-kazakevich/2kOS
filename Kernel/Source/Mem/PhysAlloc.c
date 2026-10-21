#include <Limine/LimineParse.h>
#include <Mem/PhysAlloc.h>
#include <Limine/limine.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <SpinLock.h>
#include <Types.h>

STATIC PhysAlloc GPhysAllocator;
STATIC UINT8 *PhysAllocBitmap = NULLPTR;
STATIC UINT64 PhysAllocBitmapSize = 0;

EXTERN(CHAR, KernelPhysStart);
EXTERN(CHAR, KernelPhysEnd);

// Forward decl
STATIC VOID PhysAllocMarkRangeLocked(PhysAlloc *P, UINT32 StartPage, UINT32 Count, BOOL Used);
STATIC UINT32 PhysAllocFindContinuousPagesLocked(PhysAlloc *P, UINT32 Count);
STATIC UINT32 PhysAllocFindContinuousPagesBelowLocked(PhysAlloc *P, UINT32 Count, UINT64 PhysLimit);
STATIC VOID *PhysAllocAllocatePageLocked(PhysAlloc *P);
STATIC VOID PhysAllocFreePageLocked(PhysAlloc *P, VOID *Addr);
STATIC VOID *PhysAllocAllocateRangeLocked(PhysAlloc *P, UINT32 PageCount);
STATIC VOID PhysAllocFreeRangeLocked(PhysAlloc *P, VOID *Addr, UINT32 PageCount);
STATIC VOID *PhysAllocAllocatePageBelowLocked(PhysAlloc *P, UINT64 PhysLimit);
STATIC VOID *PhysAllocAllocateRangeBelowLocked(PhysAlloc *P, UINT32 PageCount, UINT64 PhysLimit);
STATIC UINT64 PhysAllocAllocateAlignedBelowLocked(PhysAlloc *P, UINT32 Pages,
                                                  USIZE Align, UINT64 PhysLimit,
                                                  VOID **VirtOut);
STATIC VOID PhysAllocReserveRangeLocked(PhysAlloc *P, UINT64 PhysAddr, UINT64 Size);

STATIC INLINE BOOL IsPowerOfTwo(USIZE N) {
    return N != 0 && (N & (N - 1)) == 0;
}

// ============================================================================
// HELPER: FindBitmapMemory — called only from Init, before acquiring the lock
// ============================================================================

STATIC UINT64 FindBitmapMemory(struct limine_memmap_entry **Entries,
                                UINT64 EntryCount, UINT64 NeededBytes) {
    UINT64 BestAddr = 0;
    UINT64 BestSize = 0;

    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        if (Entry->type != LIMINE_MEMMAP_USABLE) continue;

        UINT64 Start = (Entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        UINT64 End = (Entry->base + Entry->length) & ~(PAGE_SIZE - 1);
        UINT64 Size = End - Start;

        if (Size >= NeededBytes && Size > BestSize) {
            if (BestAddr == 0 || Start < BestAddr) {
                BestAddr = Start;
                BestSize = Size;
            }
        }
    }

    return BestAddr;
}

// ============================================================================
// INIT — does not take the lock, called once at startup
// ============================================================================

VOID PhysAllocInit(PhysAlloc *PhysAllocator) {
    SpinLockInit(&PhysAllocator->Lock);

    if (!LimineMemoryMapAvailable()) return;

    struct limine_memmap_entry **Entries = LimineGetMemoryMapEntries();
    UINT64 EntryCount = LimineGetMemoryMapEntryCount();
    if (!Entries || EntryCount == 0) return;

    UINT64 LowestAddr = 0xFFFFFFFFFFFFFFFF;
    UINT64 HighestAddr = 0;
    UINT64 TotalUsableMemory = 0;

    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        if (Entry->type == LIMINE_MEMMAP_USABLE) {
            if (Entry->base < LowestAddr) LowestAddr = Entry->base;
            UINT64 RegionEnd = Entry->base + Entry->length;
            if (RegionEnd > HighestAddr) HighestAddr = RegionEnd;
            TotalUsableMemory += Entry->length;
        }
    }

    if (LowestAddr & (PAGE_SIZE - 1))
        LowestAddr = (LowestAddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (HighestAddr & (PAGE_SIZE - 1))
        HighestAddr = HighestAddr & ~(PAGE_SIZE - 1);

    PhysAllocator->TotalPages = (HighestAddr - LowestAddr) / PAGE_SIZE;
    PhysAllocator->UsedPages = 0;
    PhysAllocator->BaseAddr = LowestAddr;

    PhysAllocBitmapSize = (PhysAllocator->TotalPages + 7) / 8;

    UINT64 BitmapPhysAddr = FindBitmapMemory(Entries, EntryCount, PhysAllocBitmapSize);
    if (!BitmapPhysAddr) {
        for (UINT64 I = 0; I < EntryCount; I++) {
            struct limine_memmap_entry *Entry = Entries[I];
            if (Entry->type == LIMINE_MEMMAP_USABLE) {
                UINT64 Start = (Entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (Start + PhysAllocBitmapSize <= Entry->base + Entry->length) {
                    BitmapPhysAddr = Start;
                    break;
                }
            }
        }
    }
    if (!BitmapPhysAddr) return;

    UINT64 BitmapPages = (PhysAllocBitmapSize + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 BitmapEnd = BitmapPhysAddr + (BitmapPages * PAGE_SIZE);

    UINT64 HhdmOffset = LimineGetHHDMOffset();
    if (!HhdmOffset) return;

    PhysAllocBitmap = (UINT8*)(BitmapPhysAddr + HhdmOffset);
    PhysAllocator->Bitmap = PhysAllocBitmap;

    MemSet(PhysAllocator->Bitmap, 0, PhysAllocBitmapSize);
    MemSet(PhysAllocator->Bitmap, 0xFF, PhysAllocBitmapSize);
    PhysAllocator->UsedPages = PhysAllocator->TotalPages;

    // Free the USABLE memory (except the bitmap)
    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        if (Entry->type != LIMINE_MEMMAP_USABLE) continue;

        UINT64 Start = (Entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        UINT64 End = (Entry->base + Entry->length) & ~(PAGE_SIZE - 1);

        for (UINT64 Addr = Start; Addr < End; Addr += PAGE_SIZE) {
            if (Addr >= BitmapPhysAddr && Addr < BitmapEnd) continue;
            if (Addr >= LowestAddr && Addr < HighestAddr) {
                UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
                if (PageIdx < PhysAllocator->TotalPages) {
                    UINT32 ByteIdx = PageIdx / 8;
                    UINT32 Bit = PageIdx % 8;
                    PhysAllocator->Bitmap[ByteIdx] &= ~(1 << Bit);
                    PhysAllocator->UsedPages--;
                }
            }
        }
    }

    // Reserve the first 1 MB
    for (UINT64 Addr = 0; Addr < 0x100000; Addr += PAGE_SIZE) {
        if (Addr >= PhysAllocator->BaseAddr &&
            Addr < PhysAllocator->BaseAddr + (UINT64)PhysAllocator->TotalPages * PAGE_SIZE) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
                UINT32 ByteIdx = PageIdx / 8;
                UINT32 Bit = PageIdx % 8;
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
                }
            }
        }
    }

    // Reserve the kernel
    UINT64 KernelStart = (UINT64)&KernelPhysStart & ~(PAGE_SIZE - 1);
    UINT64 KernelEnd = ((UINT64)&KernelPhysEnd + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (UINT64 Addr = KernelStart; Addr < KernelEnd; Addr += PAGE_SIZE) {
        if (Addr >= PhysAllocator->BaseAddr &&
            Addr < PhysAllocator->BaseAddr + (UINT64)PhysAllocator->TotalPages * PAGE_SIZE) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
                UINT32 ByteIdx = PageIdx / 8;
                UINT32 Bit = PageIdx % 8;
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
                }
            }
        }
    }

    // Reserve the bitmap itself
    UINT64 BitmapStartAligned = BitmapPhysAddr & ~(PAGE_SIZE - 1);
    UINT64 BitmapEndAligned = (BitmapPhysAddr + PhysAllocBitmapSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (UINT64 Addr = BitmapStartAligned; Addr < BitmapEndAligned; Addr += PAGE_SIZE) {
        if (Addr >= PhysAllocator->BaseAddr &&
            Addr < PhysAllocator->BaseAddr + (UINT64)PhysAllocator->TotalPages * PAGE_SIZE) {
            UINT32 PageIdx = (Addr - PhysAllocator->BaseAddr) / PAGE_SIZE;
            if (PageIdx < PhysAllocator->TotalPages) {
                UINT32 ByteIdx = PageIdx / 8;
                UINT32 Bit = PageIdx % 8;
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    PhysAllocator->UsedPages++;
                }
            }
        }
    }
}

// ============================================================================
// INTERNAL (Locked) — assume P->Lock is already held
// ============================================================================

STATIC VOID PhysAllocMarkRangeLocked(PhysAlloc *P, UINT32 StartPage,
                                      UINT32 Count, BOOL Used) {
    for (UINT32 I = 0; I < Count; I++) {
        UINT32 PageIdx = StartPage + I;
        if (PageIdx >= P->TotalPages) break;

        UINT32 ByteIdx = PageIdx / 8;
        UINT32 Bit = PageIdx % 8;

        if (Used) {
            if (!(P->Bitmap[ByteIdx] & (1 << Bit))) {
                P->Bitmap[ByteIdx] |= (1 << Bit);
                P->UsedPages++;
            }
        } else {
            if (P->Bitmap[ByteIdx] & (1 << Bit)) {
                P->Bitmap[ByteIdx] &= ~(1 << Bit);
                P->UsedPages--;
            }
        }
    }
}

STATIC UINT32 PhysAllocFindContinuousPagesLocked(PhysAlloc *P, UINT32 Count) {
    if (!P || Count == 0 || Count > P->TotalPages) return (UINT32)-1;

    UINT32 BitmapBytes = (P->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;

    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (P->Bitmap[ByteIdx] == 0xFF) {
            Continuous = 0;
            continue;
        }

        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;
            if (PageIdx >= P->TotalPages) break;

            if (!(P->Bitmap[ByteIdx] & (1 << Bit))) {
                if (Continuous == 0) StartPage = PageIdx;
                Continuous++;
                if (Continuous == Count) return StartPage;
            } else {
                Continuous = 0;
            }
        }
    }
    return (UINT32)-1;
}

STATIC UINT32 PhysAllocFindContinuousPagesBelowLocked(PhysAlloc *P,
                                                       UINT32 Count,
                                                       UINT64 PhysLimit) {
    if (!P || Count == 0 || Count > P->TotalPages) return (UINT32)-1;
    if (PhysLimit <= P->BaseAddr) return (UINT32)-1;

    UINT64 MaxAddr = PhysLimit;
    UINT64 PmmEnd = P->BaseAddr + (UINT64)P->TotalPages * PAGE_SIZE;
    if (MaxAddr > PmmEnd) MaxAddr = PmmEnd;

    UINT32 MaxPageIdx = (UINT32)((MaxAddr - P->BaseAddr) / PAGE_SIZE);
    if (MaxPageIdx == 0) return (UINT32)-1;

    UINT32 BitmapBytes = (P->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;

    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (P->Bitmap[ByteIdx] == 0xFF) {
            Continuous = 0;
            continue;
        }

        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;

            if (PageIdx >= P->TotalPages) return (UINT32)-1;
            if (PageIdx >= MaxPageIdx) return (UINT32)-1;

            if (!(P->Bitmap[ByteIdx] & (1 << Bit))) {
                if (Continuous == 0) StartPage = PageIdx;
                Continuous++;
                if (Continuous == Count) return StartPage;
            } else {
                Continuous = 0;
            }
        }
    }
    return (UINT32)-1;
}

STATIC VOID *PhysAllocAllocatePageLocked(PhysAlloc *P) {
    if (!P || !P->Bitmap) return NULLPTR;

    UINT32 BitmapBytes = (P->TotalPages + 7) / 8;

    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (P->Bitmap[ByteIdx] == 0x00) {
            P->Bitmap[ByteIdx] = 0x01;
            UINT32 PageIdx = ByteIdx * 8;
            if (PageIdx < P->TotalPages) {
                P->UsedPages++;
                return (VOID *)(P->BaseAddr + (UINT64)PageIdx * PAGE_SIZE);
            }
        }

        if (P->Bitmap[ByteIdx] != 0xFF) {
            for (INT Bit = 0; Bit < 8; Bit++) {
                if (!(P->Bitmap[ByteIdx] & (1 << Bit))) {
                    P->Bitmap[ByteIdx] |= (1 << Bit);
                    UINT32 PageIdx = ByteIdx * 8 + Bit;
                    if (PageIdx < P->TotalPages) {
                        P->UsedPages++;
                        return (VOID *)(P->BaseAddr + (UINT64)PageIdx * PAGE_SIZE);
                    }
                }
            }
        }
    }
    return NULLPTR;
}

STATIC VOID PhysAllocFreePageLocked(PhysAlloc *P, VOID *Addr) {
    if (!P || !P->Bitmap || !Addr) return;

    UINT64 PageAddr = (UINT64)Addr & ~(PAGE_SIZE - 1);
    if (PageAddr < P->BaseAddr) return;

    UINT32 PageIdx = (PageAddr - P->BaseAddr) / PAGE_SIZE;
    if (PageIdx >= P->TotalPages) return;

    UINT32 ByteIdx = PageIdx / 8;
    UINT32 Bit = PageIdx % 8;

    if (P->Bitmap[ByteIdx] & (1 << Bit)) {
        P->Bitmap[ByteIdx] &= ~(1 << Bit);
        P->UsedPages--;
    }
}

STATIC VOID *PhysAllocAllocateRangeLocked(PhysAlloc *P, UINT32 PageCount) {
    if (!P || PageCount == 0) return NULLPTR;

    UINT32 StartPage = PhysAllocFindContinuousPagesLocked(P, PageCount);
    if (StartPage == (UINT32)-1) return NULLPTR;

    PhysAllocMarkRangeLocked(P, StartPage, PageCount, TRUE);

    UINT64 Addr = P->BaseAddr + (UINT64)StartPage * PAGE_SIZE;
    VOID *VirtAddr = (VOID*)PhysToVirt(Addr);
    MemSet(VirtAddr, 0, PageCount * PAGE_SIZE);
    return VirtAddr;
}

STATIC VOID PhysAllocFreeRangeLocked(PhysAlloc *P, VOID *Addr, UINT32 PageCount) {
    if (!P || !Addr || PageCount == 0) return;

    UINT64 PageAddr = (UINT64)Addr & ~(PAGE_SIZE - 1);
    if (PageAddr < P->BaseAddr) return;

    UINT32 StartPage = (PageAddr - P->BaseAddr) / PAGE_SIZE;
    if (StartPage >= P->TotalPages) return;

    PhysAllocMarkRangeLocked(P, StartPage, PageCount, FALSE);
}

STATIC VOID *PhysAllocAllocatePageBelowLocked(PhysAlloc *P, UINT64 PhysLimit) {
    if (!P || !P->Bitmap) return NULLPTR;

    UINT32 StartPage = PhysAllocFindContinuousPagesBelowLocked(P, 1, PhysLimit);
    if (StartPage == (UINT32)-1) return NULLPTR;

    PhysAllocMarkRangeLocked(P, StartPage, 1, TRUE);

    UINT64 Addr = P->BaseAddr + (UINT64)StartPage * PAGE_SIZE;
    VOID *Virt = (VOID *)PhysToVirt(Addr);
    MemSet(Virt, 0, PAGE_SIZE);
    return (VOID *)Addr;
}

STATIC VOID *PhysAllocAllocateRangeBelowLocked(PhysAlloc *P, UINT32 PageCount,
                                                UINT64 PhysLimit) {
    if (!P || PageCount == 0) return NULLPTR;

    UINT32 StartPage = PhysAllocFindContinuousPagesBelowLocked(P, PageCount, PhysLimit);
    if (StartPage == (UINT32)-1) return NULLPTR;

    PhysAllocMarkRangeLocked(P, StartPage, PageCount, TRUE);

    UINT64 Addr = P->BaseAddr + (UINT64)StartPage * PAGE_SIZE;
    VOID *Virt = (VOID *)PhysToVirt(Addr);
    MemSet(Virt, 0, (UINT64)PageCount * PAGE_SIZE);
    return Virt;
}

STATIC UINT64 PhysAllocAllocateAlignedBelowLocked(PhysAlloc *P, UINT32 Pages,
                                                   USIZE Align, UINT64 PhysLimit,
                                                   VOID **VirtOut) {
    if (!P) return 0;
    if (Align < PAGE_SIZE) Align = PAGE_SIZE;
    if ((Align & (Align - 1)) != 0) return 0;

    UINT32 AlignPages = (UINT32)(Align / PAGE_SIZE);
    if (AlignPages < 1) AlignPages = 1;
    UINT32 Request = Pages + AlignPages - 1;

    UINT32 StartPage = PhysAllocFindContinuousPagesBelowLocked(P, Request, PhysLimit);
    if (StartPage == (UINT32)-1) return 0;

    UINT64 PhysBase = P->BaseAddr + (UINT64)StartPage * PAGE_SIZE;
    UINT64 Aligned = (PhysBase + Align - 1) & ~((UINT64)Align - 1);
    UINT64 EndNeeded = Aligned + (UINT64)Pages * PAGE_SIZE;
    UINT64 RangeEnd = PhysBase + (UINT64)Request * PAGE_SIZE;

    if (EndNeeded > RangeEnd) return 0;

    UINT32 HeadPages = (UINT32)((Aligned - PhysBase) / PAGE_SIZE);
    UINT32 TailPages = Request - HeadPages - Pages;

    PhysAllocMarkRangeLocked(P, StartPage, Request, TRUE);

    if (HeadPages > 0)
        PhysAllocMarkRangeLocked(P, StartPage, HeadPages, FALSE);
    if (TailPages > 0)
        PhysAllocMarkRangeLocked(P, StartPage + HeadPages + Pages, TailPages, FALSE);

    VOID *Virt = (VOID *)PhysToVirt(Aligned);
    MemSet(Virt, 0, (UINT64)Pages * PAGE_SIZE);

    if (VirtOut) *VirtOut = Virt;
    return Aligned;
}

STATIC VOID PhysAllocReserveRangeLocked(PhysAlloc *P, UINT64 PhysAddr, UINT64 Size) {
    if (!P || !P->Bitmap || Size == 0) return;
    if (PhysAddr < P->BaseAddr) return;

    UINT64 End = PhysAddr + Size;
    UINT64 PmmBase = P->BaseAddr;
    UINT64 PmmEnd = PmmBase + (UINT64)P->TotalPages * PAGE_SIZE;

    if (PhysAddr >= PmmEnd) return;
    if (End > PmmEnd) End = PmmEnd;

    UINT64 StartAligned = PhysAddr & ~(PAGE_SIZE - 1);
    UINT64 EndAligned = (End + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    UINT32 StartPage = (UINT32)((StartAligned - PmmBase) / PAGE_SIZE);
    UINT32 PageCount = (UINT32)((EndAligned - StartAligned) / PAGE_SIZE);

    if (StartPage >= P->TotalPages) return;
    if (StartPage + PageCount > P->TotalPages)
        PageCount = P->TotalPages - StartPage;

    PhysAllocMarkRangeLocked(P, StartPage, PageCount, TRUE);
}

// ============================================================================
// PUBLIC API — acquire P->Lock
// ============================================================================

VOID *PhysAllocAllocatePage(PhysAlloc *P) {
    if (!P) return NULLPTR;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    VOID *R = PhysAllocAllocatePageLocked(P);
    SpinLockReleaseIrq(&P->Lock, Flags);
    return R;
}

VOID PhysAllocFreePage(PhysAlloc *P, VOID *Addr) {
    if (!P) return;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    PhysAllocFreePageLocked(P, Addr);
    SpinLockReleaseIrq(&P->Lock, Flags);
}

VOID *PhysAllocAllocateRange(PhysAlloc *P, UINT32 PageCount) {
    if (!P) return NULLPTR;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    VOID *R = PhysAllocAllocateRangeLocked(P, PageCount);
    SpinLockReleaseIrq(&P->Lock, Flags);
    return R;
}

VOID PhysAllocFreeRange(PhysAlloc *P, VOID *Addr, UINT32 PageCount) {
    if (!P) return;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    PhysAllocFreeRangeLocked(P, Addr, PageCount);
    SpinLockReleaseIrq(&P->Lock, Flags);
}

VOID *PhysAllocAllocatePageBelow(PhysAlloc *P, UINT64 PhysLimit) {
    if (!P) return NULLPTR;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    VOID *R = PhysAllocAllocatePageBelowLocked(P, PhysLimit);
    SpinLockReleaseIrq(&P->Lock, Flags);
    return R;
}

VOID *PhysAllocAllocatePageBelow4GB(PhysAlloc *P) {
    return PhysAllocAllocatePageBelow(P, PHYS_LIMIT_4GB);
}

VOID *PhysAllocAllocateRangeBelow(PhysAlloc *P, UINT32 PageCount, UINT64 PhysLimit) {
    if (!P) return NULLPTR;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    VOID *R = PhysAllocAllocateRangeBelowLocked(P, PageCount, PhysLimit);
    SpinLockReleaseIrq(&P->Lock, Flags);
    return R;
}

VOID *PhysAllocAllocateRangeBelow4GB(PhysAlloc *P, UINT32 PageCount) {
    return PhysAllocAllocateRangeBelow(P, PageCount, PHYS_LIMIT_4GB);
}

UINT64 PhysAllocAllocateAlignedBelow(PhysAlloc *P, UINT32 Pages, USIZE Align,
                                      UINT64 PhysLimit, VOID **VirtOut) {
    if (!P) return 0;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    UINT64 R = PhysAllocAllocateAlignedBelowLocked(P, Pages, Align, PhysLimit, VirtOut);
    SpinLockReleaseIrq(&P->Lock, Flags);
    return R;
}

VOID PhysAllocReserveRange(PhysAlloc *P, UINT64 PhysAddr, UINT64 Size) {
    if (!P) return;
    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);
    PhysAllocReserveRangeLocked(P, PhysAddr, Size);
    SpinLockReleaseIrq(&P->Lock, Flags);
}

// ============================================================================
// PhysAllocAllocateAligned — public, but internal calls are under the lock
// ============================================================================

VOID *PhysAllocAllocateAlignedRange(PhysAlloc *P, UINT32 PageCount, UINT32 AlignmentPages) {
    if (!P || PageCount == 0 || AlignmentPages == 0) return NULLPTR;
    if ((AlignmentPages & (AlignmentPages - 1)) != 0) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&P->Lock);

    UINT32 BitmapBytes = (P->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;
    BOOL InAligned = FALSE;
    VOID *Result = NULLPTR;

    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (P->Bitmap[ByteIdx] == 0xFF) {
            Continuous = 0;
            InAligned = FALSE;
            continue;
        }

        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;
            if (PageIdx >= P->TotalPages) break;

            if (!InAligned && (PageIdx % AlignmentPages) != 0) continue;

            if (!(P->Bitmap[ByteIdx] & (1 << Bit))) {
                if (Continuous == 0) {
                    StartPage = PageIdx;
                    InAligned = TRUE;
                }
                Continuous++;

                if (Continuous == PageCount) {
                    PhysAllocMarkRangeLocked(P, StartPage, PageCount, TRUE);
                    UINT64 Addr = P->BaseAddr + (UINT64)StartPage * PAGE_SIZE;
                    VOID *VirtAddr = (VOID *)PhysToVirt(Addr);
                    MemSet(VirtAddr, 0, PageCount * PAGE_SIZE);
                    Result = VirtAddr;
                    goto done;
                }
            } else {
                Continuous = 0;
                InAligned = FALSE;
            }
        }
    }

done:
    SpinLockReleaseIrq(&P->Lock, Flags);
    return Result;
}

UINT64 PhysAllocAllocateAligned(UINT32 Pages, USIZE Align, VOID **VirtOut) {
    if (!IsPowerOfTwo(Align) || Align < PAGE_SIZE) Align = PAGE_SIZE;

    UINT32 AlignPages = (UINT32)(Align / PAGE_SIZE);
    if (AlignPages < 1) AlignPages = 1;

    UINT32 TotalPages = Pages + AlignPages;

    for (INT Attempt = 0; Attempt < 16; Attempt++) {
        VOID *Range = PhysAllocAllocateRange(PhysAllocGet(), TotalPages);
        if (!Range) {
            TotalPages = Pages + (AlignPages >> (Attempt + 1));
            if (TotalPages < Pages) TotalPages = Pages;
            continue;
        }

        UINT64 PhysBase = VirtToPhys((UINT64)Range);
        UINT64 Aligned = (PhysBase + Align - 1) & ~((UINT64)Align - 1);
        UINT64 Offset = Aligned - PhysBase;
        UINT64 EndNeeded = Offset + (UINT64)Pages * PAGE_SIZE;

        if (EndNeeded <= (UINT64)TotalPages * PAGE_SIZE) {
            UINT32 HeadPages = (UINT32)(Offset / PAGE_SIZE);
            UINT32 TailPages = TotalPages - HeadPages - Pages;

            if (HeadPages > 0)
                PhysAllocFreeRange(PhysAllocGet(), Range, HeadPages);
            if (TailPages > 0) {
                VOID *TailStart = (VOID *)((UINT8 *)Range + (HeadPages + Pages) * PAGE_SIZE);
                PhysAllocFreeRange(PhysAllocGet(), TailStart, TailPages);
            }

            VOID *VirtAligned = (VOID *)PhysToVirt(Aligned);
            if (VirtOut) *VirtOut = VirtAligned;
            return Aligned;
        }

        PhysAllocFreeRange(PhysAllocGet(), Range, TotalPages);
        TotalPages += AlignPages;
    }

    return 0;
}

PhysAlloc* PhysAllocGet(VOID) {
    return &GPhysAllocator;
}