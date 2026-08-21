#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Mem/Paging.h>
#include <Types.h>
#include <Lib/String.h>
#include <Return.h>
#include <Limine/LimineParse.h>

/*
 * Configuration
 */
#define ALIGN 8
#define MAGIC 0xB16B00B5U
#ifndef SIZE_MAX
#define SIZE_MAX 18446744073709551615ULL
#endif
#define HEAP_MAX_SIZE (512 * 1024 * 1024)  // 512 MB maximum

static UINT64 GHeapCanary = 0;
static BOOL GCanaryInitialized = FALSE;

/*
 * Block header
 */
typedef struct BlockHeader
{
    UINT32 Magic;
    UINT64 Canary;
    USIZE Size;
    INT Free;
    struct BlockHeader *Prev;
    struct BlockHeader *Next;
} BlockHeader;

#define MIN_SPLIT_SIZE (sizeof(BlockHeader) + ALIGN)

/*
 * Global
 */
static BlockHeader *HeapHead = NULLPTR;
static BlockHeader *HeapTail = NULLPTR;
static VOID *HeapStartVirt = NULLPTR;
static VOID *HeapEndVirt = NULLPTR;
static USIZE HeapCurrentSize = 0;
static UINT64 HeapPhysBase = 0;

static inline USIZE AlignUp(USIZE N) {
    return (N + (ALIGN - 1)) & ~(ALIGN - 1);
}

static inline VOID *HeaderToPayload(BlockHeader *H) {
    return (VOID *)((CHAR *)H + sizeof(BlockHeader));
}

static inline BlockHeader *PayloadToHeader(VOID *P) {
    return (BlockHeader *)((CHAR *)P - sizeof(BlockHeader));
}

static VOID HeapCanaryInit(VOID) {
    if (GCanaryInitialized) return;
    
    UINT64 Seed = 0;
    
    UINT64 ReturnAddr;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(ReturnAddr));
    Seed ^= ReturnAddr;
    
    UINT64 StackPtr;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(StackPtr));
    Seed ^= StackPtr;
    
    UINT64 Tsc;
    __asm__ volatile ("rdtsc" : "=A"(Tsc));
    Seed ^= Tsc;
    
    UINT64 Cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(Cr3));
    Seed ^= Cr3;
    
    UINT64 State = Seed;
    State = (State * 1103515245 + 12345) & 0x7FFFFFFF;
    State = (State * 1103515245 + 12345) & 0x7FFFFFFF;
    State = (State * 1103515245 + 12345) & 0x7FFFFFFF;
    
    GHeapCanary = State;
    GHeapCanary ^= 0x9E3779B97F4A7C15ULL;
    GHeapCanary = (GHeapCanary << 13) | (GHeapCanary >> 51);
    GHeapCanary ^= 0xBF58476D1CE4E5B9ULL;
    GHeapCanary = (GHeapCanary << 17) | (GHeapCanary >> 47);
    GHeapCanary ^= 0x94D049BB133111EBULL;
    
    if (GHeapCanary == MAGIC) {
        GHeapCanary ^= 0xDEADBEEF;
    }

    if (GHeapCanary == 0) {
        GHeapCanary = 0xB16B00B5;
    }
    
    GCanaryInitialized = TRUE;
}

static inline UINT64 HeapGetCanary(VOID) {
    if (!GCanaryInitialized) {
        HeapCanaryInit();
    }
    return GHeapCanary;
}

/*
 * Expand heap using PhysAlloc
 */
static INT HeapExpand(USIZE Bytes) {
    USIZE Need = AlignUp(Bytes + sizeof(BlockHeader));
    USIZE PagesNeeded = (Need + PAGE_SIZE - 1) / PAGE_SIZE;
    USIZE ExpandSize = PagesNeeded * PAGE_SIZE;
    
    if (HeapCurrentSize + ExpandSize > HEAP_MAX_SIZE) {
        return 0;
    }
    
    // Allocate physical pages
    VOID *Virt = PhysAllocAllocateRange(PhysAllocGet(), PagesNeeded);
    if (!Virt) {
        return 0;
    }

    UINT64 PhysAddr = VirtToPhys((UINT64)Virt);
    
    // Map pages (if needed, but HHDM already maps all memory)
    // For HHDM mapping is not needed, the memory is already available via virt = phys + hhdm_offset
    
    // Create header for new block
    BlockHeader *H = (BlockHeader *)Virt;
    H->Magic = MAGIC;
    H->Canary = HeapGetCanary();
    H->Free = 1;
    H->Size = ExpandSize - sizeof(BlockHeader);
    H->Prev = HeapTail;
    H->Next = NULLPTR;
    
    if (HeapTail) {
        HeapTail->Next = H;
    } else {
        HeapHead = H;
    }
    HeapTail = H;
    
    HeapEndVirt = (VOID*)((UINTPTR)HeapEndVirt + ExpandSize);
    HeapCurrentSize += ExpandSize;
    HeapPhysBase = PhysAddr;
    
    return 1;
}

/*
 * Initialize heap - allocate initial memory via PhysAlloc
 */
INT MemoryAllocatorInit(VOID) {
    // Allocate initial memory for the heap (16 MB)
    UINT32 PagesNeeded = (HEAP_INITIAL_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    
    VOID *Virt = PhysAllocAllocateRange(PhysAllocGet(), PagesNeeded);
    if (!Virt) {
        RETURN(INCORRECT_VALUE);
    }

    HeapStartVirt = Virt;
    HeapEndVirt = (VOID *)((UINTPTR)Virt + HEAP_INITIAL_SIZE);
    HeapCurrentSize = HEAP_INITIAL_SIZE;
    HeapPhysBase = VirtToPhys((UINT64)Virt);
    
    HeapCanaryInit();
    
    HeapHead = (BlockHeader *)HeapStartVirt;
    HeapHead->Magic = MAGIC;
    HeapHead->Canary = HeapGetCanary();
    HeapHead->Size = HEAP_INITIAL_SIZE - sizeof(BlockHeader);
    HeapHead->Free = 1;
    HeapHead->Prev = HeapHead->Next = NULLPTR;
    
    HeapTail = HeapHead;
    
    RETURN(SUCCESS);
}

/*
 * Find free block
 */
static BlockHeader *FindFit(USIZE Size) {
    BlockHeader *Cur = HeapHead;
    while (Cur) {
        if (Cur->Free && Cur->Size >= Size)
            return Cur;
        Cur = Cur->Next;
    }
    return NULLPTR;
}

/*
 * Split block
 */
static VOID SplitBlock(BlockHeader *H, USIZE ReqSize) {
    if (!H) return;
    if (H->Size < ReqSize + MIN_SPLIT_SIZE) return;
    
    CHAR *NewHdrAddr = (CHAR *)HeaderToPayload(H) + ReqSize;
    BlockHeader *NewH = (BlockHeader *)NewHdrAddr;
    NewH->Magic = MAGIC;
    NewH->Canary = HeapGetCanary();
    NewH->Free = 1;
    NewH->Size = H->Size - ReqSize - sizeof(BlockHeader);
    NewH->Prev = H;
    NewH->Next = H->Next;
    if (NewH->Next) NewH->Next->Prev = NewH;
    H->Next = NewH;
    H->Size = ReqSize;
    if (HeapTail == H) HeapTail = NewH;
}

/*
 * Coalesce adjacent free blocks
 */
static VOID Coalesce(BlockHeader *H) {
    if (!H) return;
    
    if (H->Next && H->Next->Free) {
        BlockHeader *N = H->Next;
        H->Size = H->Size + sizeof(BlockHeader) + N->Size;
        H->Next = N->Next;
        if (N->Next) N->Next->Prev = H;
        if (HeapTail == N) HeapTail = H;
    }
    
    if (H->Prev && H->Prev->Free) {
        BlockHeader *P = H->Prev;
        P->Size = P->Size + sizeof(BlockHeader) + H->Size;
        P->Next = H->Next;
        if (H->Next) H->Next->Prev = P;
        if (HeapTail == H) HeapTail = P;
    }
}

/*
 * malloc
 */
VOID *MemoryAllocate(USIZE Size) {
    if (Size == 0) return NULLPTR;
    Size = AlignUp(Size);
    
    BlockHeader *Fit = FindFit(Size);
    
    while (!Fit) {
        if (!HeapExpand(Size)) {
            return NULLPTR;
        }
        Fit = FindFit(Size);
    }
    
    SplitBlock(Fit, Size);
    Fit->Free = 0;
    Fit->Canary = HeapGetCanary();
    return HeaderToPayload(Fit);
}

/*
 * free
 */
VOID MemoryFree(VOID *Ptr) {
    if (!Ptr) return;
    
    BlockHeader *H = PayloadToHeader(Ptr);
    
    if (H->Magic != MAGIC)
        return;

    if (H->Canary != HeapGetCanary()) {
        return;
    }
    
    if (H->Free) {
        return;
    }
    
    USIZE Size = H->Size;
    SecureMemZero(Ptr, Size);
    
    H->Free = 1;
    Coalesce(H);
}

/*
 * realloc
 */
VOID *MemoryReallocate(VOID *Ptr, USIZE NewSize) {
    if (!Ptr) return MemoryAllocate(NewSize);
    if (NewSize == 0) {
        MemoryFree(Ptr);
        return NULLPTR;
    }
    
    BlockHeader *H = PayloadToHeader(Ptr);
    if (H->Magic != MAGIC) return NULLPTR;
    
    NewSize = AlignUp(NewSize);
    if (NewSize <= H->Size) {
        SplitBlock(H, NewSize);
        return Ptr;
    }
    
    VOID *NewP = MemoryAllocate(NewSize);
    if (!NewP) return NULLPTR;
    
    MemCpy(NewP, Ptr, H->Size);
    
    SecureMemZero(Ptr, H->Size);
    
    MemoryFree(Ptr);
    return NewP;
}

/*
 * calloc
 */
VOID *MemoryCallocate(USIZE NMemB, USIZE Size) {
    if (NMemB != 0 && Size > SIZE_MAX / NMemB) {
        return NULLPTR;
    }
    
    USIZE TotalSize = NMemB * Size;
    if (TotalSize == 0) return NULLPTR;
    
    VOID *Ptr = MemoryAllocate(TotalSize);
    if (!Ptr) return NULLPTR;
    
    MemSet(Ptr, 0, TotalSize);
    return Ptr;
}

/*
 * Statistics
 */
VOID GetKMemoryStats(KMemoryStats *Stats) {
    if (!Stats) return;
    
    Stats->TotalManaged = 0;
    Stats->UsedPayload = 0;
    Stats->FreePayload = 0;
    Stats->LargestFree = 0;
    Stats->NumBlocks = Stats->NumUsed = Stats->NumFree = 0;
    
    BlockHeader *Cur = HeapHead;
    while (Cur) {
        Stats->NumBlocks++;
        Stats->TotalManaged += sizeof(BlockHeader) + Cur->Size;
        if (Cur->Free) {
            Stats->NumFree++;
            Stats->FreePayload += Cur->Size;
            if (Cur->Size > Stats->LargestFree)
                Stats->LargestFree = Cur->Size;
        }
        else {
            Stats->NumUsed++;
            Stats->UsedPayload += Cur->Size;
        }
        Cur = Cur->Next;
    }
}

/*
 * Aligned allocation
 */
VOID *MemoryAllocateAligned(USIZE Size, USIZE Alignment) {
    if (Alignment < ALIGN) Alignment = ALIGN;
    if ((Alignment & (Alignment - 1)) != 0) return NULLPTR;
    
    USIZE TotalSize = Size + Alignment - 1 + sizeof(VOID*);
    VOID* OriginalPtr = MemoryAllocate(TotalSize);
    if (!OriginalPtr) return NULLPTR;
    
    UINTPTR Addr = (UINTPTR)OriginalPtr + sizeof(VOID*);
    UINTPTR AlignedAddr = (Addr + Alignment - 1) & ~(Alignment - 1);
    
    VOID** PtrStore = (VOID**)(AlignedAddr - sizeof(VOID*));
    *PtrStore = OriginalPtr;
    
    return (VOID*)AlignedAddr;
}

VOID MemoryFreeAligned(VOID *Ptr) {
    if (!Ptr) return;
    
    VOID** PtrStore = (VOID**)((UINTPTR)Ptr - sizeof(VOID*));
    VOID* OriginalPtr = *PtrStore;
    
    BlockHeader *H = PayloadToHeader(OriginalPtr);
    if (H->Magic == MAGIC && !H->Free) {
        SecureMemZero(Ptr, H->Size);
    }
    
    MemoryFree(OriginalPtr);
}

/*
 * Get current heap size
 */
USIZE MemoryGetHeapSize(VOID) {
    return HeapCurrentSize;
}

/*
 * Get the virtual address of the beginning of the heap
 */
VOID* MemoryGetHeapStart(VOID) {
    return HeapStartVirt;
}