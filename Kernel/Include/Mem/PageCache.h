#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <SpinLock.h>
#include <Types.h>
#include <List.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

#define PCACHE_PAGE_SIZE        4096
#define PCACHE_HASH_BITS        12
#define PCACHE_HASH_SIZE        (1u << PCACHE_HASH_BITS)
#define PCACHE_HASH_MASK        (PCACHE_HASH_SIZE - 1)

#define PCACHE_DEFAULT_PAGES    (16 * 1024)     // 64 MB
#define PCACHE_MIN_PAGES        64

// Background writeback
#define PCACHE_WRITEBACK_MS     1000
#define PCACHE_WRITEBACK_BATCH  64

// ============================================================================
// FLAGS
// ============================================================================

#define PCACHE_FLAG_VALID       (1u << 0)
#define PCACHE_FLAG_DIRTY       (1u << 1)
#define PCACHE_FLAG_LOCKED      (1u << 2)   // is currently in use (ref > 0)
#define PCACHE_FLAG_WRITEBACK   (1u << 3)   // it’s being written to the disk now.

// ============================================================================
// STRUCTURES
// ============================================================================

typedef struct PCachePage {
    ListHead HashNode;      // In the hash bucket (by Disk+Lba)
    ListHead LruNode;       // In the global LRU
    ListHead DirtyNode;     // In the list of dirty items (if DIRTY)

    Disk     *Disk;
    UINT64    Lba;          // Initial LBA (aligned to the page)
    UINT32    SectorCount;  // How many sectors are in a page?

    UINT8     Data[PCACHE_PAGE_SIZE];

    volatile UINT32 RefCount;
    volatile UINT32 Flags;

    UINT64    LastAccess;   // Ticks of the last access

    SpinLock  Lock;
} PCachePage;

typedef struct {
    SpinLock    Lock;
    ListHead    Hash[PCACHE_HASH_SIZE];
    ListHead    Lru;              // head = the freshest, prev = the oldest
    ListHead    DirtyList;

    UINT32      PageCount;
    UINT32      MaxPages;
    UINT32      DirtyCount;

    // Statistics
    volatile UINT64 HitCount;
    volatile UINT64 MissCount;
    volatile UINT64 EvictCount;
    volatile UINT64 WritebackCount;
    volatile UINT64 ReadCount;
    volatile UINT64 WriteCount;

    // Background writeback
    volatile BOOL  WritebackRunning;
    volatile BOOL  Initialized;
    INT            WritebackPid;
} PCache;

// ============================================================================
// PUBLIC API
// ============================================================================

VOID PCacheInit(VOID);
VOID PCacheShutdown(VOID);

VOID PCacheSetSize(UINT32 MaxPages);
UINT32 PCacheGetSize(VOID);

INT PCacheRead(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buf);
INT PCacheWrite(Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buf);

INT PCacheFlush(Disk *Disk);
INT PCacheFlushAll(VOID);

VOID PCacheInvalidate(Disk *Disk, UINT64 Lba, UINT32 Count);
VOID PCacheInvalidateDisk(Disk *Disk);

VOID PCacheGetStats(UINT32 *Pages, UINT32 *Dirty, UINT32 *Max,
                    UINT64 *Hits, UINT64 *Misses,
                    UINT64 *Evicts, UINT64 *Writebacks);
VOID PCacheResetStats(VOID);