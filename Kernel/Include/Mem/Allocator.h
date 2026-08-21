#pragma once

#include <Types.h>
#include <Lib/String.h>

#define HEAP_CANARY_MAGIC 0xDEADBEEF

#define HEAP_INITIAL_SIZE (16 * 1024 * 1024)

typedef struct {
    UINT32 CanaryValue;
    BOOL Initialized;
} HeapCanary;

/*
 * Memory allocator statistics structure
 */
typedef struct {
    USIZE TotalManaged;   // Total bytes managed (payload + headers)
    USIZE UsedPayload;    // Bytes currently used by allocations
    USIZE FreePayload;    // Bytes free for allocation
    USIZE LargestFree;    // Largest contiguous free block
    USIZE NumBlocks;      // Total number of blocks (used + free)
    USIZE NumUsed;        // Number of used blocks
    USIZE NumFree;        // Number of free blocks
} KMemoryStats;

INT MemoryAllocatorInit(VOID);
VOID* MemoryAllocate(USIZE Size);
VOID MemoryFree(VOID *Ptr);
VOID* MemoryReallocate(VOID *Ptr, USIZE NewSize);
VOID* MemoryCallocate(USIZE NMemB, USIZE Size);
VOID GetKMemoryStats(KMemoryStats *Stats);
VOID* MemoryAllocateAligned(USIZE Size, USIZE Alignment);
VOID MemoryFreeAligned(VOID *Ptr);