#include <Mem/PageCache.h>
#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Basecon.h>
#include <Asm/Cpu.h>
#include <Return.h>

// ============================================================================
// GLOBAL STATE
// ============================================================================

STATIC PCache GPCache = {0};

// ============================================================================
// HASH
// ============================================================================

STATIC INLINE UINT32 PCacheHash(Disk *Disk, UINT64 Lba) {
    UINT64 H = (UINT64)(UINTPTR)Disk;
    H ^= Lba * 0x9E3779B97F4A7C15ULL;
    H ^= H >> 33;
    H *= 0xFF51AFD7ED558CCDULL;
    H ^= H >> 33;
    H *= 0xC4CEB9FE1A85EC53ULL;
    H ^= H >> 33;
    return (UINT32)(H & PCACHE_HASH_MASK);
}

// ============================================================================
// LRU
// ============================================================================

STATIC VOID PCacheLruTouch(PCachePage *Page) {
    ListDel(&Page->LruNode);
    ListAdd(&GPCache.Lru, &Page->LruNode);
    Page->LastAccess = TimerTicks();
}

// ============================================================================
// PAGE ALLOCATION
// ============================================================================

STATIC PCachePage *PCacheAllocPage(VOID) {
    PCachePage *Page = (PCachePage*)MemoryAllocate(sizeof(PCachePage));
    if (!Page) return NULLPTR;
    MemSet(Page, 0, sizeof(PCachePage));
    return Page;
}

STATIC VOID PCacheFreePage(PCachePage *Page) {
    if (Page) MemoryFree(Page);
}

// ============================================================================
// DIRECT I/O (в обход кэша)
// ============================================================================

STATIC INT PCacheReadDirect(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buf) {
    if (!Disk || !Disk->Read) RETURN(NO_OBJECT);
    return Disk->Read(Disk, Lba, Count, Buf);
}

STATIC INT PCacheWriteDirect(Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buf) {
    if (!Disk || !Disk->Write) RETURN(NO_OBJECT);
    return Disk->Write(Disk, Lba, Count, Buf);
}

// ============================================================================
// EVICTION
// ============================================================================

// Ищем жертву. Возвращает страницу с RefCount == 0, либо NULLPTR.
// Вызывается с захваченным Lock.
STATIC PCachePage *PCacheFindVictim(VOID) {
    ListHead *Pos = GPCache.Lru.Prev;
    INT Scanned = 0;
    INT MaxScan = (INT)GPCache.PageCount + 1;

    while (Pos != &GPCache.Lru && Scanned < MaxScan) {
        PCachePage *Page = ListEntry(Pos, PCachePage, LruNode);
        Pos = Pos->Prev;
        Scanned++;

        if (Page->RefCount > 0) continue;
        if (Page->Flags & PCACHE_FLAG_WRITEBACK) continue;

        return Page;
    }
    return NULLPTR;
}

// Вытесняет одну страницу. Lock НЕ захвачен.
STATIC BOOL PCacheEvictOne(VOID) {
    SpinLockAcquire(&GPCache.Lock);

    PCachePage *Victim = PCacheFindVictim();
    if (!Victim) {
        SpinLockRelease(&GPCache.Lock);
        return FALSE;
    }

    // Забираем страницу
    Victim->RefCount = 1;
    Victim->Flags |= PCACHE_FLAG_WRITEBACK;

    // Убираем из всех списков
    ListDel(&Victim->HashNode);
    ListDel(&Victim->LruNode);
    if (Victim->Flags & PCACHE_FLAG_DIRTY) {
        ListDel(&Victim->DirtyNode);
        GPCache.DirtyCount--;
    }

    BOOL WasDirty = (Victim->Flags & PCACHE_FLAG_DIRTY) != 0;
    Disk *Disk = Victim->Disk;
    UINT64 Lba = Victim->Lba;
    UINT32 Count = Victim->SectorCount;

    GPCache.PageCount--;
    GPCache.EvictCount++;

    SpinLockRelease(&GPCache.Lock);

    // Пишем вне Lock
    if (WasDirty) {
        INT Rc = PCacheWriteDirect(Disk, Lba, Count, Victim->Data);
        if (Rc == SUCCESS) {
            GPCache.WritebackCount++;
        }
    }

    PCacheFreePage(Victim);
    return TRUE;
}

// Освобождает место до PageCount < MaxPages.
STATIC INT PCacheEnsureRoom(VOID) {
    if (GPCache.MaxPages == 0) RETURN(NO_MEMORY);

    INT Attempts = 0;
    INT MaxAttempts = (INT)GPCache.MaxPages * 2;

    while (GPCache.PageCount >= GPCache.MaxPages) {
        if (!PCacheEvictOne()) {
            // Не смогли вытеснить — все заняты
            if (GPCache.PageCount >= GPCache.MaxPages) {
                RETURN(BUSY);
            }
        }
        if (++Attempts > MaxAttempts) RETURN(BUSY);
    }
    RETURN(SUCCESS);
}

// ============================================================================
// GET PAGE
// ============================================================================

STATIC PCachePage *PCacheGetPage(Disk *Disk, UINT64 Lba) {
    if (!Disk) return NULLPTR;

    UINT32 Hash = PCacheHash(Disk, Lba);

    // 1. Ищем в кэше
    SpinLockAcquire(&GPCache.Lock);

    ListHead *Pos;
    ListForEach(Pos, &GPCache.Hash[Hash]) {
        PCachePage *Page = ListEntry(Pos, PCachePage, HashNode);
        if (Page->Disk == Disk && Page->Lba == Lba) {
            Page->RefCount++;
            PCacheLruTouch(Page);
            GPCache.HitCount++;
            SpinLockRelease(&GPCache.Lock);
            return Page;
        }
    }

    GPCache.MissCount++;
    SpinLockRelease(&GPCache.Lock);

    // 2. Освобождаем место
    if (PCacheEnsureRoom() != SUCCESS) {
        return NULLPTR;
    }

    // 3. Аллоцируем
    PCachePage *New = PCacheAllocPage();
    if (!New) return NULLPTR;

    New->Disk = Disk;
    New->Lba = Lba;
    New->SectorCount = PCACHE_PAGE_SIZE / Disk->SectorSize;

    // 4. Читаем с диска (вне Lock)
    INT Rc = PCacheReadDirect(Disk, Lba, New->SectorCount, New->Data);
    if (Rc != SUCCESS) {
        PCacheFreePage(New);
        return NULLPTR;
    }

    // 5. Вставляем в кэш
    SpinLockAcquire(&GPCache.Lock);

    // Проверяем ещё раз — может кто-то вставил, пока мы читали
    ListForEach(Pos, &GPCache.Hash[Hash]) {
        PCachePage *Page = ListEntry(Pos, PCachePage, HashNode);
        if (Page->Disk == Disk && Page->Lba == Lba) {
            Page->RefCount++;
            PCacheLruTouch(Page);
            SpinLockRelease(&GPCache.Lock);
            PCacheFreePage(New);
            return Page;
        }
    }

    New->RefCount = 1;
    New->Flags = PCACHE_FLAG_VALID;
    ListAdd(&GPCache.Hash[Hash], &New->HashNode);
    ListAdd(&GPCache.Lru, &New->LruNode);
    New->LastAccess = TimerTicks();
    GPCache.PageCount++;

    SpinLockRelease(&GPCache.Lock);
    return New;
}

STATIC VOID PCacheRelease(PCachePage *Page) {
    if (!Page) return;
    SpinLockAcquire(&GPCache.Lock);
    if (Page->RefCount > 0) Page->RefCount--;
    SpinLockRelease(&GPCache.Lock);
}

// ============================================================================
// READ / WRITE
// ============================================================================

INT PCacheRead(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buf) {
    if (!Disk || !Buf || Count == 0) RETURN(INCORRECT_VALUE);

    if (!GPCache.Initialized) {
        return PCacheReadDirect(Disk, Lba, Count, Buf);
    }

    UINT32 BytesPerSector = Disk->SectorSize;
    UINT64 ByteOffset = Lba * BytesPerSector;
    UINT64 ByteCount = (UINT64)Count * BytesPerSector;
    UINT64 EndByte = ByteOffset + ByteCount;

    UINT8 *Out = (UINT8*)Buf;
    UINT64 Cur = ByteOffset;
    UINT32 SectorsPerPage = PCACHE_PAGE_SIZE / BytesPerSector;

    while (Cur < EndByte) {
        UINT64 PageLba = (Cur / PCACHE_PAGE_SIZE) * SectorsPerPage;

        PCachePage *Page = PCacheGetPage(Disk, PageLba);
        if (!Page) {
            // Fallback — читаем напрямую
            UINT64 CurLba = Cur / BytesPerSector;
            UINT32 Left = (UINT32)((EndByte - Cur) / BytesPerSector);
            UINT32 This = Left;
            if (This > 128) This = 128;
            INT Rc = PCacheReadDirect(Disk, CurLba, This, Out);
            if (Rc != SUCCESS) RETURN(Rc);
            Out += This * BytesPerSector;
            Cur += This * BytesPerSector;
            continue;
        }

        UINT64 PageStart = PageLba * BytesPerSector;
        UINT32 InPageOff = (UINT32)(Cur - PageStart);
        UINT32 Chunk = PCACHE_PAGE_SIZE - InPageOff;
        if (Chunk > (UINT32)(EndByte - Cur)) Chunk = (UINT32)(EndByte - Cur);

        MemCpy(Out, Page->Data + InPageOff, Chunk);
        PCacheRelease(Page);

        Out += Chunk;
        Cur += Chunk;
    }

    GPCache.ReadCount++;
    RETURN(SUCCESS);
}

INT PCacheWrite(Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buf) {
    if (!Disk || !Buf || Count == 0) RETURN(INCORRECT_VALUE);

    if (!GPCache.Initialized) {
        return PCacheWriteDirect(Disk, Lba, Count, Buf);
    }

    UINT32 BytesPerSector = Disk->SectorSize;
    UINT64 ByteOffset = Lba * BytesPerSector;
    UINT64 ByteCount = (UINT64)Count * BytesPerSector;
    UINT64 EndByte = ByteOffset + ByteCount;

    const UINT8 *In = (const UINT8*)Buf;
    UINT64 Cur = ByteOffset;
    UINT32 SectorsPerPage = PCACHE_PAGE_SIZE / BytesPerSector;

    while (Cur < EndByte) {
        UINT64 PageLba = (Cur / PCACHE_PAGE_SIZE) * SectorsPerPage;

        PCachePage *Page = PCacheGetPage(Disk, PageLba);
        if (!Page) {
            // Fallback — пишем напрямую
            UINT64 CurLba = Cur / BytesPerSector;
            UINT32 Left = (UINT32)((EndByte - Cur) / BytesPerSector);
            UINT32 This = Left;
            if (This > 128) This = 128;
            INT Rc = PCacheWriteDirect(Disk, CurLba, This, In);
            if (Rc != SUCCESS) RETURN(Rc);
            In += This * BytesPerSector;
            Cur += This * BytesPerSector;
            continue;
        }

        UINT64 PageStart = PageLba * BytesPerSector;
        UINT32 InPageOff = (UINT32)(Cur - PageStart);
        UINT32 Chunk = PCACHE_PAGE_SIZE - InPageOff;
        if (Chunk > (UINT32)(EndByte - Cur)) Chunk = (UINT32)(EndByte - Cur);

        MemCpy(Page->Data + InPageOff, In, Chunk);

        // Помечаем грязной
        SpinLockAcquire(&GPCache.Lock);
        if (!(Page->Flags & PCACHE_FLAG_DIRTY)) {
            Page->Flags |= PCACHE_FLAG_DIRTY;
            ListAdd(&GPCache.DirtyList, &Page->DirtyNode);
            GPCache.DirtyCount++;
        }
        SpinLockRelease(&GPCache.Lock);

        PCacheRelease(Page);

        In += Chunk;
        Cur += Chunk;
    }

    GPCache.WriteCount++;
    RETURN(SUCCESS);
}

// ============================================================================
// FLUSH
// ============================================================================

// Внутренний: сбросить грязные страницы. Disk == NULLPTR → все диски.
STATIC INT PCacheFlushInternal(Disk *Disk) {
    if (!GPCache.Initialized) RETURN(SUCCESS);

    INT Flushed = 0;

    for (INT Pass = 0; Pass < 2; Pass++) {
        SpinLockAcquire(&GPCache.Lock);

        ListHead *Pos, *Next;
        ListForEachSafe(Pos, Next, &GPCache.DirtyList) {
            PCachePage *Page = ListEntry(Pos, PCachePage, DirtyNode);

            if (Disk && Page->Disk != Disk) continue;
            if (Page->RefCount > 0) continue;
            if (Page->Flags & PCACHE_FLAG_WRITEBACK) continue;

            Page->RefCount = 1;
            Page->Flags |= PCACHE_FLAG_WRITEBACK;

            SpinLockRelease(&GPCache.Lock);

            INT Rc = PCacheWriteDirect(Page->Disk, Page->Lba,
                                       Page->SectorCount, Page->Data);

            SpinLockAcquire(&GPCache.Lock);

            if (Rc == SUCCESS) {
                Page->Flags &= ~(PCACHE_FLAG_DIRTY | PCACHE_FLAG_WRITEBACK);
                ListDel(&Page->DirtyNode);
                GPCache.DirtyCount--;
                GPCache.WritebackCount++;
                Flushed++;
            } else {
                Page->Flags &= ~PCACHE_FLAG_WRITEBACK;
            }
            Page->RefCount = 0;
        }

        SpinLockRelease(&GPCache.Lock);

        if (GPCache.DirtyCount == 0) break;
    }

    if (Disk && Disk->Flush) {
        Disk->Flush(Disk);
    }

    RETURN(Flushed);
}

INT PCacheFlush(Disk *Disk) {
    if (!Disk) RETURN(NO_OBJECT);
    return PCacheFlushInternal(Disk);
}

INT PCacheFlushAll(VOID) {
    return PCacheFlushInternal(NULLPTR);
}

// ============================================================================
// INVALIDATE
// ============================================================================

VOID PCacheInvalidate(Disk *Disk, UINT64 Lba, UINT32 Count) {
    if (!Disk || !GPCache.Initialized) return;

    UINT32 SectorsPerPage = PCACHE_PAGE_SIZE / Disk->SectorSize;
    UINT64 StartPage = (Lba / SectorsPerPage) * SectorsPerPage;
    UINT64 EndLba = Lba + Count;

    SpinLockAcquire(&GPCache.Lock);

    for (UINT64 Cur = StartPage; Cur < EndLba; Cur += SectorsPerPage) {
        UINT32 Hash = PCacheHash(Disk, Cur);
        ListHead *Pos, *Next;
        ListForEachSafe(Pos, Next, &GPCache.Hash[Hash]) {
            PCachePage *Page = ListEntry(Pos, PCachePage, HashNode);
            if (Page->Disk == Disk && Page->Lba == Cur) {
                if (Page->RefCount > 0) {
                    Page->Flags &= ~PCACHE_FLAG_VALID;
                    continue;
                }
                ListDel(&Page->HashNode);
                ListDel(&Page->LruNode);
                if (Page->Flags & PCACHE_FLAG_DIRTY) {
                    ListDel(&Page->DirtyNode);
                    GPCache.DirtyCount--;
                }
                GPCache.PageCount--;
                PCacheFreePage(Page);
                break;
            }
        }
    }

    SpinLockRelease(&GPCache.Lock);
}

VOID PCacheInvalidateDisk(Disk *Disk) {
    if (!Disk || !GPCache.Initialized) return;

    SpinLockAcquire(&GPCache.Lock);

    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GPCache.Lru) {
        PCachePage *Page = ListEntry(Pos, PCachePage, LruNode);
        if (Page->Disk != Disk) continue;
        if (Page->RefCount > 0) {
            Page->Flags &= ~PCACHE_FLAG_VALID;
            continue;
        }
        ListDel(&Page->HashNode);
        ListDel(&Page->LruNode);
        if (Page->Flags & PCACHE_FLAG_DIRTY) {
            ListDel(&Page->DirtyNode);
            GPCache.DirtyCount--;
        }
        GPCache.PageCount--;
        PCacheFreePage(Page);
    }

    SpinLockRelease(&GPCache.Lock);
}

// ============================================================================
// WRITEBACK THREAD
// ============================================================================

STATIC VOID PCacheWritebackThread(VOID) {
    BaseconPrintf(BASECON_TYPE_NORMAL, "pcache: writeback thread started\n");

    while (GPCache.WritebackRunning) {
        TimerSleep(PCACHE_WRITEBACK_MS);
        if (!GPCache.WritebackRunning) break;

        INT Flushed = 0;
        SpinLockAcquire(&GPCache.Lock);

        ListHead *Pos, *Next;
        ListForEachSafe(Pos, Next, &GPCache.DirtyList) {
            if (Flushed >= PCACHE_WRITEBACK_BATCH) break;

            PCachePage *Page = ListEntry(Pos, PCachePage, DirtyNode);
            if (Page->RefCount > 0) continue;
            if (Page->Flags & PCACHE_FLAG_WRITEBACK) continue;

            Page->RefCount = 1;
            Page->Flags |= PCACHE_FLAG_WRITEBACK;

            SpinLockRelease(&GPCache.Lock);
            INT Rc = PCacheWriteDirect(Page->Disk, Page->Lba,
                                       Page->SectorCount, Page->Data);
            SpinLockAcquire(&GPCache.Lock);

            if (Rc == SUCCESS) {
                Page->Flags &= ~(PCACHE_FLAG_DIRTY | PCACHE_FLAG_WRITEBACK);
                ListDel(&Page->DirtyNode);
                GPCache.DirtyCount--;
                GPCache.WritebackCount++;
                Flushed++;
            } else {
                Page->Flags &= ~PCACHE_FLAG_WRITEBACK;
            }
            Page->RefCount = 0;
        }

        SpinLockRelease(&GPCache.Lock);
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "pcache: writeback thread stopped\n");
    TaskExit(0);
}

// ============================================================================
// INIT / SHUTDOWN
// ============================================================================

VOID PCacheInit(VOID) {
    if (GPCache.Initialized) return;

    MemSet(&GPCache, 0, sizeof(GPCache));
    SpinLockInit(&GPCache.Lock);

    for (UINT32 I = 0; I < PCACHE_HASH_SIZE; I++) {
        ListInit(&GPCache.Hash[I]);
    }
    ListInit(&GPCache.Lru);
    ListInit(&GPCache.DirtyList);

    GPCache.MaxPages = PCACHE_DEFAULT_PAGES;
    GPCache.Initialized = TRUE;

    // Запускаем writeback
    GPCache.WritebackRunning = TRUE;
    INT Pid = TaskCreate(PCacheWritebackThread, KSTACK_SIZE, "pcache-wb");
    if (Pid < 0) {
        GPCache.WritebackRunning = FALSE;
        BaseconPrintf(BASECON_TYPE_WARNING, "pcache: failed to start writeback\n");
    } else {
        GPCache.WritebackPid = Pid;
    }

    BaseconPrintf(BASECON_TYPE_NORMAL, "pcache: initialized (%u pages, %u MB)\n",
                  GPCache.MaxPages,
                  (UINT32)((UINT64)GPCache.MaxPages * PCACHE_PAGE_SIZE / (1024*1024)));
}

VOID PCacheShutdown(VOID) {
    if (!GPCache.Initialized) return;

    GPCache.WritebackRunning = FALSE;
    PCacheFlushAll();

    SpinLockAcquire(&GPCache.Lock);
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &GPCache.Lru) {
        PCachePage *Page = ListEntry(Pos, PCachePage, LruNode);
        ListDel(&Page->HashNode);
        ListDel(&Page->LruNode);
        PCacheFreePage(Page);
    }
    GPCache.PageCount = 0;
    GPCache.DirtyCount = 0;
    SpinLockRelease(&GPCache.Lock);

    GPCache.Initialized = FALSE;
}

// ============================================================================
// CONFIG
// ============================================================================

VOID PCacheSetSize(UINT32 MaxPages) {
    if (MaxPages < PCACHE_MIN_PAGES) MaxPages = PCACHE_MIN_PAGES;

    SpinLockAcquire(&GPCache.Lock);
    GPCache.MaxPages = MaxPages;
    SpinLockRelease(&GPCache.Lock);

    while (GPCache.PageCount > MaxPages) {
        if (!PCacheEvictOne()) break;
    }
}

UINT32 PCacheGetSize(VOID) {
    return GPCache.MaxPages;
}

// ============================================================================
// STATS
// ============================================================================

VOID PCacheGetStats(UINT32 *Pages, UINT32 *Dirty, UINT32 *Max,
                    UINT64 *Hits, UINT64 *Misses,
                    UINT64 *Evicts, UINT64 *Writebacks) {
    SpinLockAcquire(&GPCache.Lock);
    if (Pages)      *Pages = GPCache.PageCount;
    if (Dirty)      *Dirty = GPCache.DirtyCount;
    if (Max)        *Max = GPCache.MaxPages;
    if (Hits)       *Hits = GPCache.HitCount;
    if (Misses)     *Misses = GPCache.MissCount;
    if (Evicts)     *Evicts = GPCache.EvictCount;
    if (Writebacks) *Writebacks = GPCache.WritebackCount;
    SpinLockRelease(&GPCache.Lock);
}

VOID PCacheResetStats(VOID) {
    SpinLockAcquire(&GPCache.Lock);
    GPCache.HitCount = 0;
    GPCache.MissCount = 0;
    GPCache.EvictCount = 0;
    GPCache.WritebackCount = 0;
    GPCache.ReadCount = 0;
    GPCache.WriteCount = 0;
    SpinLockRelease(&GPCache.Lock);
}