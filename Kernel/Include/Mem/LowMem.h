#pragma once

#include <Types.h>

#define LOWMEM_MAX_ADDR     0x1000000ULL    // 16 MB
#define LOWMEM_SIZE         (256 * 1024)    // 256 KB reserved
#define LOWMEM_ALIGN        0x10000ULL      // 64 KB

// Initialization. Call IT ONCE, BEFORE PhysAllocInit.
// Will find a contiguous region below 16 MB, aligned to 64 KB,
//and she'll remember it. The PMM will not know about these pages.
VOID LowMemInit(VOID);

// Allocate N pages below 16 MB (64 KB alignment).
// Returns the HHDM virtual address or NULLPTR.
VOID* LowMemAllocate(UINT32 PageCount);

// Release (no-op for bump allocator, but there is an API).
VOID LowMemFree(VOID *Addr, UINT32 PageCount);

// The physical address of the HHDM pointer.
UINT64 LowMemPhys(VOID *VirtAddr);

// How much is available.
UINT32 LowMemFreePages(VOID);

UINT64 LowMemPhysBase(VOID);