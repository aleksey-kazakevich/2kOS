#pragma once

#include <SpinLock.h>
#include <Types.h>

// Page size - 4KB (standard for x86_64))
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12
#define PHYS_LIMIT_4GB  (0x100000000ULL)
#define PMM_BITMAP_MAX_SIZE (64 * 1024 * 1024)

// Physical memory manager structure
typedef struct {
    UINT8* Bitmap;        // Pointer to a bit array
    UINT64 BaseAddr;     // Starting physical address
    UINT32 TotalPages;   // Total pages under management
    UINT32 UsedPages;    // Occupied pages
    UINT64 BitmapSize;   // Bitmap size in bytes
    UINT64 LegacyDmaPhys;    // Physical address of reserved region
    UINT64 LegacyDmaVirt;    // HHDM virtual address
    UINT32 LegacyDmaPages;   // Total pages
    UINT32 LegacyDmaUsed;    // Used pages
    SpinLock Lock;
} PhysAlloc;

VOID PhysAllocInit(PhysAlloc *PhysAllocator);

VOID PhysAllocReserveRange(PhysAlloc *PhysAllocator, UINT64 PhysAddr, UINT64 Size);

VOID* PhysAllocAllocatePage(PhysAlloc *PhysAllocator);

UINT64 PhysAllocAllocateAligned(UINT32 Pages, USIZE Align, VOID **VirtOut);

VOID PhysAllocFreePage(PhysAlloc *PhysAllocator, VOID* Addr);

VOID* PhysAllocAllocateRange(PhysAlloc *PhysAllocator, UINT32 PageCount);

VOID PhysAllocFreeRange(PhysAlloc *PhysAllocator, VOID *Addr, UINT32 PageCount);

VOID* PhysAllocAllocateAlignedRange(PhysAlloc *PhysAllocator, UINT32 PageCount, UINT32 AlignmentPages);

// Statistics
UINT32 PhysAllocGetLegacyDmaFreePages(VOID);

// Allocate one page below 4 GB.
// Returns a physical address (VOID*) or NULLPTR.
VOID *PhysAllocAllocatePageBelow4GB(PhysAlloc *PhysAllocator);

// Allocate one page below the arbitrary physical limit.
VOID *PhysAllocAllocatePageBelow(PhysAlloc *PhysAllocator, UINT64 PhysLimit);

// Allocate a continuous range from PageCount of pages below 4 GB.
// Returns an HHDM virtual address (as in PhysAllocAllocateRange) or NULLPTR.
VOID *PhysAllocAllocateRangeBelow4GB(PhysAlloc *PhysAllocator, UINT32 PageCount);

// Allocate a continuous range of PageCount pages below the limit.
VOID *PhysAllocAllocateRangeBelow(PhysAlloc *PhysAllocator,
                                   UINT32 PageCount, UINT64 PhysLimit);

// Allocate one page with alignment according to Align below the limit.
// Returns the physical address; *VirtOut is populated with the HHDM address.
UINT64 PhysAllocAllocateAlignedBelow(PhysAlloc *PhysAllocator,
                                      UINT32 Pages, USIZE Align,
                                      UINT64 PhysLimit, VOID **VirtOut);

// Free for pages allocated below the limit (the same as PhysAllocFreePage).
VOID PhysAllocFreePageBelow(PhysAlloc *PhysAllocator, VOID *Addr);

PhysAlloc* PhysAllocGet(VOID);

STATIC INLINE UINT32 PhysAllocGetFreePages(PhysAlloc *PhysAllocator) {
    return PhysAllocator ? (PhysAllocator->TotalPages - PhysAllocator->UsedPages) : 0;
}

STATIC INLINE UINT32 PhysAllocGetUsedPages(PhysAlloc *PhysAllocator) {
    return PhysAllocator ? PhysAllocator->UsedPages : 0;
}