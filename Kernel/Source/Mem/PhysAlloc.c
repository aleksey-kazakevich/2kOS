#include <Mem/PhysAlloc.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Types.h>
#include <Limine/LimineParse.h>
#include <Limine/limine.h>

// Dynamic bitmap is now a pointer, not an array
static PhysAlloc GPhysAllocator;
static UINT8 *PhysAllocBitmap = NULLPTR;  // Dynamic pointer
static UINT64 PhysAllocBitmapSize = 0;    // Bitmap size in bytes

EXTERN(CHAR, KernelPhysStart);
EXTERN(CHAR, KernelPhysEnd);

// ============================================================================
// Helper function: find a free contiguous area for a bitmap
// ============================================================================

static UINT64 FindBitmapMemory(struct limine_memmap_entry **Entries, UINT64 EntryCount, UINT64 NeededBytes) {
    UINT64 BestAddr = 0;
    UINT64 BestSize = 0;
    
    // Looking for the largest contiguous area of ​​USABLE memory
    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        if (Entry->type != LIMINE_MEMMAP_USABLE) continue;
        
        UINT64 Start = (Entry->base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        UINT64 End = (Entry->base + Entry->length) & ~(PAGE_SIZE - 1);
        UINT64 Size = End - Start;
        
        // We are looking for an area that can accommodate a bitmap
        if (Size >= NeededBytes && Size > BestSize) {
            // We prefer addresses closer to the beginning (so as not to waste memory)
            if (BestAddr == 0 || Start < BestAddr) {
                BestAddr = Start;
                BestSize = Size;
            }
        }
    }
    
    return BestAddr;
}

// ============================================================================
// Initialization with dynamic bitmap
// ============================================================================

VOID PhysAllocInit(PhysAlloc *PhysAllocator) {
    // 1. Get a memory card from Limine
    if (!LimineMemoryMapAvailable()) {
        return;
    }
    
    struct limine_memmap_entry **Entries = LimineGetMemoryMapEntries();
    UINT64 EntryCount = LimineGetMemoryMapEntryCount();
    
    if (!Entries || EntryCount == 0) {
        return;
    }
    
    // 2. Find the LOWEST and HIGHEST addresses of available memory
    UINT64 LowestAddr = 0xFFFFFFFFFFFFFFFF;
    UINT64 HighestAddr = 0;
    UINT64 TotalUsableMemory = 0;
    
    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        if (Entry->type == LIMINE_MEMMAP_USABLE) {
            if (Entry->base < LowestAddr)
                LowestAddr = Entry->base;
            
            UINT64 RegionEnd = Entry->base + Entry->length;
            if (RegionEnd > HighestAddr)
                HighestAddr = RegionEnd;
            
            TotalUsableMemory += Entry->length;
        }
    }
    
    // 3. Align to the page border
    if (LowestAddr & (PAGE_SIZE - 1))
        LowestAddr = (LowestAddr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    if (HighestAddr & (PAGE_SIZE - 1))
        HighestAddr = HighestAddr & ~(PAGE_SIZE - 1);
    
    // 4. Initialize the structure
    PhysAllocator->TotalPages = (HighestAddr - LowestAddr) / PAGE_SIZE;
    PhysAllocator->UsedPages = 0;
    PhysAllocator->BaseAddr = LowestAddr;
    
    // 5. Calculate the size of the bitmap
    PhysAllocBitmapSize = (PhysAllocator->TotalPages + 7) / 8;
    
    // 6. Find a place for the bitmap
    UINT64 BitmapPhysAddr = FindBitmapMemory(Entries, EntryCount, PhysAllocBitmapSize);
    if (!BitmapPhysAddr) {
        // Fallback: looking for any free memory
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
    
    if (!BitmapPhysAddr) {
        // Critical error - no space for bitmap
        return;
    }
    
    // 7. Резервируем страницы для битмапа
    UINT64 BitmapPages = (PhysAllocBitmapSize + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 BitmapEnd = BitmapPhysAddr + (BitmapPages * PAGE_SIZE);
    
    // 8. Get the virtual address of the bitmap via HHDM
    UINT64 HhdmOffset = LimineGetHHDMOffset();
    if (!HhdmOffset) {
        return;
    }
    
    PhysAllocBitmap = (UINT8*)(BitmapPhysAddr + HhdmOffset);
    PhysAllocator->Bitmap = PhysAllocBitmap;
    
    // 9. Clear the bitmap
    MemSet(PhysAllocator->Bitmap, 0, PhysAllocBitmapSize);
    
    // 10. Mark ALL pages as busy
    MemSet(PhysAllocator->Bitmap, 0xFF, PhysAllocBitmapSize);
    PhysAllocator->UsedPages = PhysAllocator->TotalPages;
    
    // 11. Free up USABLE memory (except for the bitmap)
    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *Entry = Entries[I];
        if (Entry->type == LIMINE_MEMMAP_USABLE) {
            UINT64 Start = Entry->base;
            UINT64 End = Entry->base + Entry->length;
            
            // Align
            Start = (Start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            End = End & ~(PAGE_SIZE - 1);
            
            for (UINT64 Addr = Start; Addr < End; Addr += PAGE_SIZE) {
                // Пропускаем область битмапа
                if (Addr >= BitmapPhysAddr && Addr < BitmapEnd) {
                    continue;
                }
                
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
    }
    
    // 12. We reserve MANDATORY areas:
    
    // First 1MB (BIOS, bootloader data)
    for (UINT64 Addr = 0; Addr < 0x100000; Addr += PAGE_SIZE) {
        if (Addr >= PhysAllocator->BaseAddr && 
            Addr < PhysAllocator->BaseAddr + PhysAllocator->TotalPages * PAGE_SIZE) {
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
    
    // Core
    UINT64 KernelStart = (UINT64)&KernelPhysStart & ~(PAGE_SIZE - 1);
    UINT64 KernelEnd = ((UINT64)&KernelPhysEnd + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (UINT64 Addr = KernelStart; Addr < KernelEnd; Addr += PAGE_SIZE) {
        if (Addr >= PhysAllocator->BaseAddr && 
            Addr < PhysAllocator->BaseAddr + PhysAllocator->TotalPages * PAGE_SIZE) {
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
    
    // PMM bitmap (we reserve ourselves)
    UINT64 BitmapStartAligned = BitmapPhysAddr & ~(PAGE_SIZE - 1);
    UINT64 BitmapEndAligned = (BitmapPhysAddr + PhysAllocBitmapSize + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (UINT64 Addr = BitmapStartAligned; Addr < BitmapEndAligned; Addr += PAGE_SIZE) {
        if (Addr >= PhysAllocator->BaseAddr && 
            Addr < PhysAllocator->BaseAddr + PhysAllocator->TotalPages * PAGE_SIZE) {
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
// Остальные функции (без изменений)
// ============================================================================

VOID* PhysAllocAllocatePage(PhysAlloc *PhysAllocator) {
    if (!PhysAllocator || !PhysAllocator->Bitmap) return NULLPTR;
    
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    
    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (PhysAllocator->Bitmap[ByteIdx] == 0x00) {
            PhysAllocator->Bitmap[ByteIdx] = 0x01;
            UINT32 PageIdx = ByteIdx * 8;
            
            if (PageIdx < PhysAllocator->TotalPages) {
                PhysAllocator->UsedPages++;
                UINT64 Addr = PhysAllocator->BaseAddr + (PageIdx * PAGE_SIZE);
                return (VOID*)Addr;
            }
        }
        
        if (PhysAllocator->Bitmap[ByteIdx] != 0xFF) {
            for (INT Bit = 0; Bit < 8; Bit++) {
                if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                    PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                    UINT32 PageIdx = ByteIdx * 8 + Bit;
                    
                    if (PageIdx < PhysAllocator->TotalPages) {
                        PhysAllocator->UsedPages++;
                        UINT64 Addr = PhysAllocator->BaseAddr + (PageIdx * PAGE_SIZE);
                        return (VOID*)Addr;
                    }
                }
            }
        }
    }
    
    return NULLPTR;
}

VOID PhysAllocFreePage(PhysAlloc *PhysAllocator, VOID *Addr) {
    if (!PhysAllocator || !PhysAllocator->Bitmap || !Addr) return;
    
    UINT64 PageAddr = (UINT64)Addr & ~(PAGE_SIZE - 1);
    
    if (PageAddr < PhysAllocator->BaseAddr) return;
    
    UINT32 PageIdx = (PageAddr - PhysAllocator->BaseAddr) / PAGE_SIZE;
    if (PageIdx >= PhysAllocator->TotalPages) return;
    
    UINT32 ByteIdx = PageIdx / 8;
    UINT32 Bit = PageIdx % 8;
    
    if (PhysAllocator->Bitmap[ByteIdx] & (1 << Bit)) {
        PhysAllocator->Bitmap[ByteIdx] &= ~(1 << Bit);
        PhysAllocator->UsedPages--;
    }
}

static UINT32 PhysAllocFindContinuousPages(PhysAlloc *PhysAllocator, UINT32 Count) {
    if (!PhysAllocator || Count == 0 || Count > PhysAllocator->TotalPages) return (UINT32)-1;
    
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;
    
    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (PhysAllocator->Bitmap[ByteIdx] == 0xFF) {
            Continuous = 0;
            continue;
        }
        
        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;
            if (PageIdx >= PhysAllocator->TotalPages) break;
            
            if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                if (Continuous == 0) {
                    StartPage = PageIdx;
                }
                Continuous++;
                
                if (Continuous == Count) {
                    return StartPage;
                }
            } else {
                Continuous = 0;
            }
        }
    }
    
    return (UINT32)-1;
}

static VOID PhysAllocMarkRange(PhysAlloc *PhysAllocator, UINT32 StartPage, UINT32 Count, BOOL Used) {
    for (UINT32 I = 0; I < Count; I++) {
        UINT32 PageIdx = StartPage + I;
        if (PageIdx >= PhysAllocator->TotalPages) break;
        
        UINT32 ByteIdx = PageIdx / 8;
        UINT32 Bit = PageIdx % 8;
        
        if (Used) {
            if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                PhysAllocator->Bitmap[ByteIdx] |= (1 << Bit);
                PhysAllocator->UsedPages++;
            }
        } else {
            if (PhysAllocator->Bitmap[ByteIdx] & (1 << Bit)) {
                PhysAllocator->Bitmap[ByteIdx] &= ~(1 << Bit);
                PhysAllocator->UsedPages--;
            }
        }
    }
}

VOID* PhysAllocAllocateRange(PhysAlloc *PhysAllocator, UINT32 PageCount) {
    if (!PhysAllocator || PageCount == 0) return NULLPTR;
    
    UINT32 StartPage = PhysAllocFindContinuousPages(PhysAllocator, PageCount);
    if (StartPage == (UINT32)-1) {
        return NULLPTR;
    }
    
    PhysAllocMarkRange(PhysAllocator, StartPage, PageCount, TRUE);
    
    UINT64 Addr = PhysAllocator->BaseAddr + (StartPage * PAGE_SIZE);
    VOID *VirtAddr = (VOID*)PhysToVirt(Addr);
    MemSet(VirtAddr, 0, PageCount * PAGE_SIZE);
    return VirtAddr;
}

VOID PhysAllocFreeRange(PhysAlloc *PhysAllocator, VOID *Addr, UINT32 PageCount) {
    if (!PhysAllocator || !Addr || PageCount == 0) return;
    
    UINT64 PageAddr = (UINT64)Addr & ~(PAGE_SIZE - 1);
    if (PageAddr < PhysAllocator->BaseAddr) return;
    
    UINT32 StartPage = (PageAddr - PhysAllocator->BaseAddr) / PAGE_SIZE;
    if (StartPage >= PhysAllocator->TotalPages) return;
    
    PhysAllocMarkRange(PhysAllocator, StartPage, PageCount, FALSE);
}

VOID* PhysAllocAllocateAlignedRange(PhysAlloc *PhysAllocator, UINT32 PageCount, UINT32 AlignmentPages) {
    if (!PhysAllocator || PageCount == 0 || AlignmentPages == 0) return NULLPTR;
    
    if ((AlignmentPages & (AlignmentPages - 1)) != 0) return NULLPTR;
    
    UINT32 BitmapBytes = (PhysAllocator->TotalPages + 7) / 8;
    UINT32 Continuous = 0;
    UINT32 StartPage = 0;
    BOOL InAligned = FALSE;
    
    for (UINT32 ByteIdx = 0; ByteIdx < BitmapBytes; ByteIdx++) {
        if (PhysAllocator->Bitmap[ByteIdx] == 0xFF) {
            Continuous = 0;
            InAligned = FALSE;
            continue;
        }
        
        for (INT Bit = 0; Bit < 8; Bit++) {
            UINT32 PageIdx = ByteIdx * 8 + Bit;
            if (PageIdx >= PhysAllocator->TotalPages) break;
            
            if (!InAligned && (PageIdx % AlignmentPages) != 0) {
                continue;
            }
            
            if (!(PhysAllocator->Bitmap[ByteIdx] & (1 << Bit))) {
                if (Continuous == 0) {
                    StartPage = PageIdx;
                    InAligned = TRUE;
                }
                Continuous++;
                
                if (Continuous == PageCount) {
                    PhysAllocMarkRange(PhysAllocator, StartPage, PageCount, TRUE);
                    UINT64 Addr = PhysAllocator->BaseAddr + (StartPage * PAGE_SIZE);
                    VOID *VirtAddr = (VOID*)(UINTPTR)Addr;
                    MemSet(VirtAddr, 0, PageCount * PAGE_SIZE);
                    return VirtAddr;
                }
            } else {
                Continuous = 0;
                InAligned = FALSE;
            }
        }
    }
    
    return NULLPTR;
}

PhysAlloc* PhysAllocGet(VOID) {
    return &GPhysAllocator;
}