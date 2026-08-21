#pragma once

#include <Types.h>

// Page size - 4KB (standard for x86_64))
#define PAGE_SIZE 4096
#define PAGE_SHIFT 12

#define PMM_BITMAP_MAX_SIZE (64 * 1024 * 1024)

// Physical memory manager structure
typedef struct {
    UINT8* Bitmap;        // Pointer to a bit array
    UINT64 BaseAddr;     // Starting physical address
    UINT32 TotalPages;   // Total pages under management
    UINT32 UsedPages;    // Occupied pages
    UINT64 BitmapSize;   // Bitmap size in bytes
} PhysAlloc;

VOID PhysAllocInit(PhysAlloc *PhysAllocator);

VOID* PhysAllocAllocatePage(PhysAlloc *PhysAllocator);

VOID PhysAllocFreePage(PhysAlloc *PhysAllocator, VOID* Addr);

VOID* PhysAllocAllocateRange(PhysAlloc *PhysAllocator, UINT32 PageCount);

VOID PhysAllocFreeRange(PhysAlloc *PhysAllocator, VOID *Addr, UINT32 PageCount);

VOID* PhysAllocAllocateAlignedRange(PhysAlloc *PhysAllocator, UINT32 PageCount, UINT32 AlignmentPages);

PhysAlloc* PhysAllocGet(VOID);

static inline UINT32 PhysAllocGetFreePages(PhysAlloc *PhysAllocator) {
    return PhysAllocator ? (PhysAllocator->TotalPages - PhysAllocator->UsedPages) : 0;
}

static inline UINT32 PhysAllocGetUsedPages(PhysAlloc *PhysAllocator) {
    return PhysAllocator ? PhysAllocator->UsedPages : 0;
}