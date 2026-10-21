#include <Limine/LimineParse.h>
#include <Limine/limine.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Mem/LowMem.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <Return.h>

#define LOWMEM_PAGE_SIZE    4096
#define LOWMEM_ALIGN_PAGES  (LOWMEM_ALIGN / LOWMEM_PAGE_SIZE)

typedef struct {
    BOOL   Initialized;
    UINT64 PhysBase;
    UINT64 VirtBase;
    UINT32 TotalPages;
    UINT32 UsedPages;
    SpinLock Lock;
} LowMemState;

STATIC LowMemState GLowMem = {0};

// ============================================================================
// Search for a free region below 16 MB
// ============================================================================

STATIC UINT64 LowMemFindRegion(struct limine_memmap_entry **Entries,
                                UINT64 EntryCount) {
    UINT32 NeedPages = LOWMEM_SIZE / LOWMEM_PAGE_SIZE;
    UINT64 BestPhys = 0;

    for (UINT64 I = 0; I < EntryCount; I++) {
        struct limine_memmap_entry *E = Entries[I];
        if (E->type != LIMINE_MEMMAP_USABLE) continue;

        UINT64 Start = E->base;
        UINT64 End   = E->base + E->length;

        if (End > LOWMEM_MAX_ADDR) End = LOWMEM_MAX_ADDR;
        if (Start >= End) continue;

        Start = (Start + LOWMEM_ALIGN - 1) & ~(LOWMEM_ALIGN - 1);

        if (Start + (UINT64)NeedPages * LOWMEM_PAGE_SIZE > End) continue;

        BestPhys = Start;
        break;
    }

    return BestPhys;
}

// ============================================================================
// Init
// ============================================================================

VOID LowMemInit(VOID) {
    if (GLowMem.Initialized) return;

    if (!LimineMemoryMapAvailable()) {
        BaseconPrintf(BASECON_TYPE_WARNING, "lowmem: no memory map\n");
        return;
    }

    struct limine_memmap_entry **Entries = LimineGetMemoryMapEntries();
    UINT64 EntryCount = LimineGetMemoryMapEntryCount();

    if (!Entries || EntryCount == 0) {
        BaseconPrintf(BASECON_TYPE_WARNING, "lowmem: empty memory map\n");
        return;
    }

    UINT64 Phys = LowMemFindRegion(Entries, EntryCount);
    if (!Phys) {
        BaseconPrintf(BASECON_TYPE_WARNING,
                      "lowmem: no region below 16 MB for legacy DMA\n");
        return;
    }

    UINT64 Hhdm = LimineGetHHDMOffset();
    if (!Hhdm) {
        BaseconPrintf(BASECON_TYPE_WARNING, "lowmem: no HHDM\n");
        return;
    }

    SpinLockInit(&GLowMem.Lock);

    GLowMem.Initialized = TRUE;
    GLowMem.PhysBase    = Phys;
    GLowMem.VirtBase    = Phys + Hhdm;
    GLowMem.TotalPages  = LOWMEM_SIZE / LOWMEM_PAGE_SIZE;
    GLowMem.UsedPages   = 0;

    MemSet((VOID *)GLowMem.VirtBase, 0, LOWMEM_SIZE);

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "lowmem: reserved %u KB at phys 0x%llX (below 16 MB)\n",
                  (UINT32)(LOWMEM_SIZE / 1024),
                  Phys);
}

// ============================================================================
// Alloc / Free
// ============================================================================

VOID* LowMemAllocate(UINT32 PageCount) {
    if (!GLowMem.Initialized) return NULLPTR;
    if (PageCount == 0) return NULLPTR;

    UINT32 Rounded = (PageCount + LOWMEM_ALIGN_PAGES - 1) & ~(LOWMEM_ALIGN_PAGES - 1);

    UINTPTR Flags = SpinLockAcquireIrq(&GLowMem.Lock);

    if (GLowMem.UsedPages + Rounded > GLowMem.TotalPages) {
        SpinLockReleaseIrq(&GLowMem.Lock, Flags);
        return NULLPTR;
    }

    UINT64 Offset = (UINT64)GLowMem.UsedPages * LOWMEM_PAGE_SIZE;
    VOID *Virt = (VOID *)(GLowMem.VirtBase + Offset);

    GLowMem.UsedPages += Rounded;

    SpinLockReleaseIrq(&GLowMem.Lock, Flags);

    // Zeroing out of the lock
    MemSet(Virt, 0, (UINT64)Rounded * LOWMEM_PAGE_SIZE);

    return Virt;
}

VOID LowMemFree(VOID *Addr, UINT32 PageCount) {
    (VOID)Addr;
    (VOID)PageCount;
    // Bump allocator — does not release
}

UINT64 LowMemPhys(VOID *VirtAddr) {
    if (!GLowMem.Initialized || !VirtAddr) return 0;

    UINTPTR Virt = (UINTPTR)VirtAddr;
    UINTPTR Base = (UINTPTR)GLowMem.VirtBase;
    UINTPTR End  = Base + ((UINT64)GLowMem.TotalPages * LOWMEM_PAGE_SIZE);

    if (Virt < Base || Virt >= End) return 0;

    return GLowMem.PhysBase + (Virt - Base);
}

UINT32 LowMemFreePages(VOID) {
    if (!GLowMem.Initialized) return 0;
    return GLowMem.TotalPages - GLowMem.UsedPages;
}

UINT64 LowMemPhysBase(VOID) {
    return GLowMem.Initialized ? GLowMem.PhysBase : 0;
}