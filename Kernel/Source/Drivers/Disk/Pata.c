#include <Drivers/Disk/Pata.h>
#include <Asm/Io.h>
#include <Lib/String.h>
#include <Idt.h>
#include <Drivers/Apic/Ioapic.h>
#include <Types.h>
#include <InterruptList.h>
#include <KDriver.h>
#include <Drivers/Apic/Apic.h>
#include <Return.h>
#include <Mem/PhysAlloc.h>
#include <SpinLock.h>
#include <Mem/Paging.h>
#include <Time/Timer.h>
#include <Drivers/Disk/DiskMgr.h>

/*
 * ============================================================================
 * Global State
 * ============================================================================
 */
static volatile PataDrive* PrimaryIrqDrive = NULLPTR;
static volatile PataDrive* SecondaryIrqDrive = NULLPTR;

static PataRequest *PrimaryRequestQueue = NULLPTR;
static PataRequest *PrimaryRequestTail = NULLPTR;
static PataRequest *SecondaryRequestQueue = NULLPTR;
static PataRequest *SecondaryRequestTail = NULLPTR;

static SpinLock GPrimaryQueueLock;
static SpinLock GSecondaryQueueLock;

// NEW: Статистика
static UINT32 GDmaFailures = 0;
static UINT32 GPioFallbacks = 0;

PataDrive PataDrives[4];
static Disk GPataDiskObjects[4];

/*
 * ============================================================================
 * Helper Functions
 * ============================================================================
 */
static inline VOID IoDelay(UINT16 CtrlPort) {
    Inb(CtrlPort + PATA_ALTSTATUS);
    Inb(CtrlPort + PATA_ALTSTATUS);
    Inb(CtrlPort + PATA_ALTSTATUS);
    Inb(CtrlPort + PATA_ALTSTATUS);
}

VOID PataDisableInterrupts(UINT16 CtrlPort) {
    Outb(CtrlPort + PATA_CONTROL, 0x02);
}

VOID PataEnableInterrupts(UINT16 CtrlPort) {
    Outb(CtrlPort + PATA_CONTROL, 0x00);
}

static INT WaitBsyClear(UINT16 BasePort, UINT16 CtrlPort, UINT32 Timeout) {
    for (UINT32 I = 0; I < Timeout; ++I) {
        UINT8 S = Inb(BasePort + PATA_STATUS);
        if (!(S & PATA_STATUS_BSY))
            RETURN(SUCCESS);
        if ((I & 0xFF) == 0)
            IoDelay(CtrlPort);
    }
    RETURN(TIMEOUT);
}

static INT WaitDrqOrErr(UINT16 BasePort, UINT16 CtrlPort, UINT32 Timeout) {
    for (UINT32 I = 0; I < Timeout; ++I) {
        UINT8 S = Inb(BasePort + PATA_STATUS);
        if (S & PATA_STATUS_ERR)
            RETURN(DEVICE_ERROR);
        if (!(S & PATA_STATUS_BSY) && (S & PATA_STATUS_DRQ))
            RETURN(SUCCESS);
        if ((I & 0xFF) == 0)
            IoDelay(CtrlPort);
    }
    RETURN(TIMEOUT);
}

static INT CheckErrAndClear(UINT16 BasePort) {
    UINT8 S = Inb(BasePort + PATA_STATUS);
    if (S & PATA_STATUS_ERR) {
        Inb(BasePort + PATA_ERROR);
        RETURN(DEVICE_ERROR);
    }
    RETURN(SUCCESS);
}

static VOID SelectDeviceAndDelay(UINT16 Base, UINT16 Ctrl, UINT8 Drive, INT LbaFlag, UINT8 HeadHigh4) {
    UINT8 Value = (LbaFlag ? 0xE0 : 0xA0) | ((Drive & 1) << 4) | (HeadHigh4 & 0x0F);
    Outb(Base + PATA_SELECT, Value);
    IoDelay(Ctrl);
}

static UINT64 IdentWordsToUINT64(const UINT16 Ident[256], INT W) {
    UINT64 V = 0;
    V |= (UINT64)Ident[W + 0];
    V |= (UINT64)Ident[W + 1] << 16;
    V |= (UINT64)Ident[W + 2] << 32;
    V |= (UINT64)Ident[W + 3] << 48;
    return V;
}

static inline INT IsAligned2(const VOID *Ptr) {
    return (((UINTPTR)Ptr) & 1u) == 0;
}

static VOID ReadSectorWordsTo(UINT16 Base, UINT16 AlignedWords[256]) {
    for (INT I = 0; I < 256; ++I)
        AlignedWords[I] = Inw(Base + PATA_DATA);
}

/*
 * ============================================================================
 * Improved PRDT Management
 * ============================================================================
 */
static PrdtEntry* PataAllocPrdt(VOID *PhysBuffer, UINT32 SectorCount, UINT16 *PrdtPhys) {
    UINT32 EntriesNeeded = (SectorCount + 62) / 63;
    if (EntriesNeeded > PRDT_ENTRY_COUNT || EntriesNeeded == 0)
        return NULLPTR;
    
    // Allocate PRDT (one page)
    PrdtEntry *Prdt = (PrdtEntry*)PhysAllocAllocatePage(PhysAllocGet());
    if (!Prdt)
        return NULLPTR;
    
    // Clear page
    MemSet(Prdt, 0, 4096);
    
    UINTPTR PhysAddrPrdt = (UINTPTR)VirtToPhysPtr(Prdt);
    
    // PRDT must be in lower 4GB and aligned
    if (PhysAddrPrdt > 0xFFFFFFFF || (PhysAddrPrdt & 3)) {
        PhysAllocFreePage(PhysAllocGet(), Prdt);
        return NULLPTR;
    }
    
    *PrdtPhys = (UINT16)PhysAddrPrdt;
    
    // Fill PRDT entries with boundary checking
    UINT32 RemainingSectors = SectorCount;
    UINT32 CurrentSector = 0;
    UINT32 EntryIndex = 0;
    
    while (RemainingSectors > 0 && EntryIndex < PRDT_ENTRY_COUNT) {
        UINT32 SectorsThisEntry = MIN(RemainingSectors, 63);
        UINT32 BytesThisEntry = SectorsThisEntry * 512;
        
        UINTPTR PhysAddr = (UINTPTR)VirtToPhysPtr((UINT8*)PhysBuffer + CurrentSector * 512);
        
        // Check for 64KB boundary crossing
        UINT32 BytesToBoundary = 0x10000 - (PhysAddr & 0xFFFF);
        if (BytesThisEntry > BytesToBoundary) {
            BytesThisEntry = BytesToBoundary;
            SectorsThisEntry = BytesThisEntry / 512;
            if (SectorsThisEntry == 0) {
                // Physical address not aligned to sector boundary
                PhysAllocFreePage(PhysAllocGet(), Prdt);
                return NULLPTR;
            }
        }
        
        Prdt[EntryIndex].PhysAddr = (UINT32)PhysAddr;
        Prdt[EntryIndex].ByteCount = (UINT16)(BytesThisEntry - 1);  // 0-based
        Prdt[EntryIndex].EndOfTable = (RemainingSectors <= SectorsThisEntry) ? PRDT_EOT : 0;
        
        RemainingSectors -= SectorsThisEntry;
        CurrentSector += SectorsThisEntry;
        EntryIndex++;
    }
    
    if (RemainingSectors > 0) {
        // Too many entries needed
        PhysAllocFreePage(PhysAllocGet(), Prdt);
        return NULLPTR;
    }
    
    return Prdt;
}

static VOID PataFreePrdt(PrdtEntry *Prdt) {
    if (Prdt) {
        PhysAllocFreePage(PhysAllocGet(), Prdt);
    }
}

/*
 * ============================================================================
 * LBA Register Setup
 * ============================================================================
 */
static VOID SetupLba28Regs(UINT16 Base, UINT16 Ctrl, UINT32 Lba, UINT8 Count, UINT8 Drive) {
    SelectDeviceAndDelay(Base, Ctrl, Drive, 1, (UINT8)((Lba >> 24) & 0x0F));
    Outb(Base + PATA_NSECT, Count);
    Outb(Base + PATA_SECTOR, (UINT8)(Lba & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 8) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 16) & 0xFF));
}

static VOID SetupLba48Regs(UINT16 Base, UINT16 Ctrl, UINT64 Lba, UINT16 Count, UINT8 Drive) {
    SelectDeviceAndDelay(Base, Ctrl, Drive, 1, (UINT8)((Lba >> 24) & 0x0F));
    IoDelay(Ctrl);

    Outb(Base + PATA_NSECT, (UINT8)((Count >> 8) & 0xFF));
    Outb(Base + PATA_SECTOR, (UINT8)((Lba >> 24) & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 32) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 40) & 0xFF));
    Outb(Base + PATA_NSECT, (UINT8)(Count & 0xFF));
    Outb(Base + PATA_SECTOR, (UINT8)(Lba & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 8) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 16) & 0xFF));
}

/*
 * ============================================================================
 * PIO Fallback (improved with error checking)
 * ============================================================================
 */
static INT PataPioReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);
    
    UINT8 *UserBuf = (UINT8 *)Buffer;
    UINT16 TmpSectorWords[256];
    UINT32 Remaining = Count;
    
    while (Remaining > 0) {
        // Setup registers
        if (Drive->SupportsLba48 && (Lba > 0x0FFFFFFF || Count > 255)) {
            UINT16 ChunkCount = (UINT16)MIN(Remaining, 65535);
            SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Lba, ChunkCount, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_READ_SECTORS_EXT);
        } else {
            UINT32 Lba32 = (UINT32)Lba;
            UINT8 ChunkCount = (UINT8)MIN(Remaining, 255);
            SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, Lba32, ChunkCount, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_READ_SECTORS);
        }
        
        // Wait for data
        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            RETURN(TIMEOUT);
        
        if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            RETURN(DEVICE_ERROR);
        
        // Read 256 words (512 bytes)
        UINT16 *Dest = (UINT16*)(UserBuf + (Count - Remaining) * 512);
        if (IsAligned2(Dest)) {
            for (INT I = 0; I < 256; I++)
                Dest[I] = Inw(Drive->BasePort + PATA_DATA);
        } else {
            ReadSectorWordsTo(Drive->BasePort, TmpSectorWords);
            MemCpy(Dest, TmpSectorWords, 512);
        }
        
        // Check for errors after read
        if (CheckErrAndClear(Drive->BasePort) != 0)
            RETURN(DEVICE_ERROR);
        
        Lba++;
        Remaining--;
    }
    
    Drive->TotalReads++;
    RETURN(SUCCESS);
}

static INT PataPioWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);
    
    const UINT8 *UserBuf = (const UINT8 *)Buffer;
    UINT16 TmpSectorWords[256];
    UINT32 Remaining = Count;
    
    while (Remaining > 0) {
        // Setup registers
        if (Drive->SupportsLba48 && (Lba > 0x0FFFFFFF || Count > 255)) {
            UINT16 ChunkCount = (UINT16)MIN(Remaining, 65535);
            SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Lba, ChunkCount, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_WRITE_SECTORS_EXT);
        } else {
            UINT32 Lba32 = (UINT32)Lba;
            UINT8 ChunkCount = (UINT8)MIN(Remaining, 255);
            SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, Lba32, ChunkCount, Drive->Drive);
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_WRITE_SECTORS);
        }
        
        // Wait for ready
        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            RETURN(TIMEOUT);
        
        if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            RETURN(DEVICE_ERROR);
        
        // Write 256 words (512 bytes)
        const UINT16 *Src = (const UINT16*)(UserBuf + (Count - Remaining) * 512);
        if (IsAligned2(Src)) {
            for (INT I = 0; I < 256; I++)
                Outw(Drive->BasePort + PATA_DATA, Src[I]);
        } else {
            MemCpy(TmpSectorWords, Src, 512);
            for (INT I = 0; I < 256; I++)
                Outw(Drive->BasePort + PATA_DATA, TmpSectorWords[I]);
        }
        
        // Wait for write completion
        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
            RETURN(TIMEOUT);
        
        if (CheckErrAndClear(Drive->BasePort) != 0)
            RETURN(DEVICE_ERROR);
        
        Lba++;
        Remaining--;
    }
    
    Drive->TotalWrites++;
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Improved DMA Operations
 * ============================================================================
 */
static INT PataDmaTransfer(PataDrive *Drive, PataRequest *Req) {
    if (!Drive || !Req || !Drive->BusMasterBase || Req->Count == 0)
        RETURN(DEVICE_INVALID);
    
    if (Req->Count > MAX_DMA_SECTORS)
        RETURN(INCORRECT_VALUE);
    
    // Allocate PRDT
    Req->Prdt = PataAllocPrdt(Req->Buffer, Req->Count, &Req->PrdtPhys);
    if (!Req->Prdt)
        RETURN(NO_MEMORY);
    
    // Reset DMA controller
    Outb(Drive->BusMasterBase + BMCR, BMCR_RESET);
    IoDelay(Drive->CtrlPort);
    
    // Wait for reset to complete
    for (INT I = 0; I < 1000; I++) {
        if (!(Inb(Drive->BusMasterBase + BMCR) & BMCR_RESET))
            break;
        IoDelay(Drive->CtrlPort);
    }
    
    // Clear status
    Outb(Drive->BusMasterBase + BMSR, BMSR_INTR | BMSR_ERROR);
    
    // Set PRDT address
    Outl(Drive->BusMasterBase + BMIDETBL, Req->PrdtPhys);
    
    // Setup LBA registers
    BOOL UseLba48 = Drive->SupportsLba48 && (Req->Lba > 0x0FFFFFFF || Req->Count > 255);
    UINT8 Command;
    
    if (UseLba48) {
        SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Req->Lba, (UINT16)Req->Count, Drive->Drive);
        Command = (Req->Op == PATA_OP_READ) ? PATA_CMD_READ_DMA_EXT : PATA_CMD_WRITE_DMA_EXT;
    } else {
        SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, (UINT32)Req->Lba, (UINT8)Req->Count, Drive->Drive);
        Command = (Req->Op == PATA_OP_READ) ? PATA_CMD_READ_DMA : PATA_CMD_WRITE_DMA;
    }
    
    // Set DMA direction
    UINT8 Bmcr = (Req->Op == PATA_OP_READ) ? BMCR_READ : 0;
    Outb(Drive->BusMasterBase + BMCR, Bmcr);
    
    // Send command
    Outb(Drive->BasePort + PATA_COMMAND, Command);
    IoDelay(Drive->CtrlPort);
    
    // Wait for DRQ or error before starting DMA
    for (INT I = 0; I < PATA_TIMEOUT_LOOPS; I++) {
        UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
        UINT8 BmsrStatus = Inb(Drive->BusMasterBase + BMSR);
        
        if (BmsrStatus & BMSR_ERROR) {
            PataFreePrdt(Req->Prdt);
            Req->Prdt = NULLPTR;
            RETURN(DEVICE_ERROR);
        }
        
        if (BmsrStatus & BMSR_DRQ) {
            // Start DMA engine
            Outb(Drive->BusMasterBase + BMCR, Bmcr | BMCR_START);
            Req->DmaActive = TRUE;
            Drive->DmaInProgress = TRUE;
            RETURN(SUCCESS);
        }
        
        if ((I & 0xFF) == 0)
            IoDelay(Drive->CtrlPort);
    }
    
    // Timeout waiting for DRQ
    PataFreePrdt(Req->Prdt);
    Req->Prdt = NULLPTR;
    RETURN(TIMEOUT);
}

/*
 * ============================================================================
 * Request Queue Management (improved)
 * ============================================================================
 */
static VOID PataQueueRequest(PataDrive *Drive, PataRequest *Req) {
    SpinLock *Lock;
    PataRequest **Queue;
    PataRequest **Tail;
    
    if (Drive->Channel == PATA_CHANNEL_PRIMARY) {
        Lock = &GPrimaryQueueLock;
        Queue = &PrimaryRequestQueue;
        Tail = &PrimaryRequestTail;
    } else {
        Lock = &GSecondaryQueueLock;
        Queue = &SecondaryRequestQueue;
        Tail = &SecondaryRequestTail;
    }
    
    SpinLockAcquire(Lock);
    Req->Next = NULLPTR;
    if (*Tail) {
        (*Tail)->Next = Req;
    } else {
        *Queue = Req;
    }
    *Tail = Req;
    SpinLockRelease(Lock);
}

static PataRequest* PataDequeueRequest(PataDrive *Drive) {
    SpinLock *Lock;
    PataRequest **Queue;
    PataRequest **Tail;
    
    if (Drive->Channel == PATA_CHANNEL_PRIMARY) {
        Lock = &GPrimaryQueueLock;
        Queue = &PrimaryRequestQueue;
        Tail = &PrimaryRequestTail;
    } else {
        Lock = &GSecondaryQueueLock;
        Queue = &SecondaryRequestQueue;
        Tail = &SecondaryRequestTail;
    }
    
    SpinLockAcquire(Lock);
    PataRequest *Req = *Queue;
    if (Req) {
        *Queue = Req->Next;
        if (!*Queue)
            *Tail = NULLPTR;
        Req->Next = NULLPTR;
    }
    SpinLockRelease(Lock);
    
    return Req;
}

/*
 * ============================================================================
 * IRQ Completion (improved with memory cleanup)
 * ============================================================================
 */
static VOID PataCompleteIrqRequest(PataDrive *Drive) {
    if (!Drive)
        return;
    
    PataRequest *Req = PataDequeueRequest(Drive);
    if (!Req)
        return;
    
    // Check DMA status
    UINT8 Bmsr = Inb(Drive->BusMasterBase + BMSR);
    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
    
    // Stop DMA
    Outb(Drive->BusMasterBase + BMCR, 0);
    Drive->DmaInProgress = FALSE;
    
    if (Req->DmaActive) {
        if (Req->Result == CANCELED) {
            PataFreePrdt(Req->Prdt);
            Req->Prdt = NULLPTR;
            Req->DmaActive = FALSE;
        } else {
            if ((Bmsr & BMSR_ERROR) || (Status & PATA_STATUS_ERR)) {
                // DMA failed, fallback to PIO
                GDmaFailures++;
                Drive->DmaFailures++;
                GPioFallbacks++;
                Drive->PioFallbacks++;
                
                if (Req->Op == PATA_OP_READ) {
                    Req->Result = PataPioReadSectors(Drive, Req->Lba, Req->Count, Req->Buffer);
                } else {
                    Req->Result = PataPioWriteSectors(Drive, Req->Lba, Req->Count, Req->Buffer);
                }
            } else {
                Req->Result = 0;
            }
        }
        
        // Free PRDT
        PataFreePrdt(Req->Prdt);
        Req->Prdt = NULLPTR;
        Req->DmaActive = FALSE;
    } else {
        Req->Result = 0;
    }
    
    // Clear interrupt status
    Outb(Drive->BusMasterBase + BMSR, Bmsr | BMSR_INTR | BMSR_ERROR);
    
    // Mark as completed
    Req->Completed = TRUE;
    
    return;
}

/*
 * ============================================================================
 * IRQ Handlers
 * ============================================================================
 */
VOID PataPrimaryIrqHandler(VOID) {
    if (PrimaryIrqDrive) {
        UINT8 Status = Inb(PATA_BASE_PRIMARY + PATA_STATUS);
        (VOID)Status;
        
        PrimaryIrqDrive->IrqPending = 1;
        PrimaryIrqDrive->IrqCount++;
    }
    
    PataCompleteIrqRequest((PataDrive *)PrimaryIrqDrive);
    ApicEoi();
}

VOID PataSecondaryIrqHandler(VOID) {
    if (SecondaryIrqDrive) {
        UINT8 Status = Inb(PATA_BASE_SECONDARY + PATA_STATUS);
        (VOID)Status;
        
        SecondaryIrqDrive->IrqPending = 1;
        SecondaryIrqDrive->IrqCount++;
    }
    
    PataCompleteIrqRequest((PataDrive *)SecondaryIrqDrive);
    ApicEoi();
}

/*
 * ============================================================================
 * Public API - Async I/O (improved)
 * ============================================================================
 */
INT PataReadSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count, 
                         VOID *Buffer, PataRequest *Req) {
    if (!Drive || !Buffer || !Req || Count == 0)
        RETURN(DEVICE_INVALID);
    
    if (Drive->TotalSectors && Lba + Count > Drive->TotalSectors)
        RETURN(DEVICE_INVALID);
    
    // Initialize request
    MemSet(Req, 0, sizeof(PataRequest));
    Req->Op = PATA_OP_READ;
    Req->Lba = Lba;
    Req->Count = Count;
    Req->Buffer = Buffer;
    Req->Completed = FALSE;
    Req->DmaActive = FALSE;
    Req->Result = -1;
    Req->Prdt = NULLPTR;
    SpinLockInit(&Req->Lock);
    
    // Try DMA first
    INT Result = PataDmaTransfer(Drive, Req);
    if (Result != 0) {
        // DMA not available, use PIO synchronously
        Req->Result = PataPioReadSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }
    
    // Queue request for IRQ completion
    PataQueueRequest(Drive, Req);
    RETURN(SUCCESS);  // Async
}

INT PataWriteSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count,
                          const VOID *Buffer, PataRequest *Req) {
    if (!Drive || !Buffer || !Req || Count == 0)
        RETURN(DEVICE_INVALID);
    
    if (Drive->TotalSectors && Lba + Count > Drive->TotalSectors)
        RETURN(DEVICE_INVALID);
    
    // Initialize request
    MemSet(Req, 0, sizeof(PataRequest));
    Req->Op = PATA_OP_WRITE;
    Req->Lba = Lba;
    Req->Count = Count;
    Req->Buffer = (VOID*)Buffer;
    Req->Completed = FALSE;
    Req->DmaActive = FALSE;
    Req->Result = -1;
    Req->Prdt = NULLPTR;
    SpinLockInit(&Req->Lock);
    
    // Try DMA first
    INT Result = PataDmaTransfer(Drive, Req);
    if (Result != 0) {
        // DMA not available, use PIO synchronously
        Req->Result = PataPioWriteSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }
    
    // Queue request for IRQ completion
    PataQueueRequest(Drive, Req);
    RETURN(SUCCESS);  // Async
}

/*
 * ============================================================================
 * Improved Wait and Cancel functions
 * ============================================================================
 */
INT PataWaitRequest(PataRequest *Req, UINT32 TimeoutMs) {
    if (!Req)
        RETURN(DEVICE_INVALID);
    
    if (Req->Completed)
        return Req->Result;
    
    UINT64 StartTicks = TimerTicks();
    UINT64 TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    
    while (!Req->Completed) {
        // Check timeout
        if (TimeoutMs > 0 && (TimerTicks() - StartTicks) >= TimeoutTicks) {
            // Cancel the request on timeout
            PataCancelRequest(Req);
            return -TIMEOUT;
        }
        CpuPause();
    }
    
    return Req->Result;
}

INT PataCancelRequest(PataRequest *Req) {
    if (!Req)
        RETURN(DEVICE_INVALID);
    
    if (Req->Completed)
        RETURN(SUCCESS);
    
    // If DMA is active, do not free the PRDT; the IRQ handler will do that
    if (Req->DmaActive) {
        Req->Result = CANCELED;
        // Just mark it as cancelled, the real cleaning will be done by IRQ
    } else {
        Req->Completed = TRUE;
        Req->Result = CANCELED;
        if (Req->Prdt) {
            PataFreePrdt(Req->Prdt);
            Req->Prdt = NULLPTR;
        }
    }
    
    RETURN(SUCCESS);
}

BOOL PataIsRequestComplete(PataRequest *Req) {
    return Req ? Req->Completed : TRUE;
}

/*
 * ============================================================================
 * Synchronous API (improved)
 * ============================================================================
 */
INT PataReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    PataRequest Req;
    INT Result = PataReadSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;
    
    return PataWaitRequest(&Req, PATA_DMA_TIMEOUT_MS);
}

INT PataWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    PataRequest Req;
    INT Result = PataWriteSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;
    
    return PataWaitRequest(&Req, PATA_DMA_TIMEOUT_MS);
}

/*
 * ============================================================================
 * Flush Cache
 * ============================================================================
 */
INT PataFlushCache(PataDrive *Drive) {
    if (!Drive)
        return -DEVICE_INVALID;
    
    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
        return -TIMEOUT;
    
    if (Drive->SupportsLba48) {
        Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_CACHE_FLUSH_EXT);
    } else {
        Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_CACHE_FLUSH);
    }
    
    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != 0)
        return -TIMEOUT;
    
    return CheckErrAndClear(Drive->BasePort);
}

/*
 * ============================================================================
 * IDENTIFY (improved)
 * ============================================================================
 */
INT PataIdentify(PataDrive *Drive, UINT16 IdentBuffer[256]) {
    if (!Drive || !IdentBuffer)
        RETURN(DEVICE_INVALID);
    
    Drive->Type = PATA_TYPE_NONE;
    Drive->SupportsLba48 = 0;
    Drive->SectorSize = 512;
    Drive->TotalSectors = 0;
    Drive->IrqPending = 0;
    Drive->IrqCount = 0;
    
    // Device Selection (CHS)
    SelectDeviceAndDelay(Drive->BasePort, Drive->CtrlPort, Drive->Drive, 0, 0);
    
    // Sending IDENTIFY
    Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY);
    IoDelay(Drive->CtrlPort);
    
    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
    if (Status == 0)
        RETURN(DEVICE_ERROR);
    
    // If ERR - possible ATAPI
    if (Status & PATA_STATUS_ERR) {
        UINT8 Cl = Inb(Drive->BasePort + PATA_LCYL);
        UINT8 Ch = Inb(Drive->BasePort + PATA_HCYL);
        if ((Cl == 0x14 && Ch == 0xEB) || (Cl == 0x69 && Ch == 0x96)) {
            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY_PACKET);
            if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != SUCCESS)
                RETURN(TIMEOUT);
            if (CheckErrAndClear(Drive->BasePort) != SUCCESS)
                RETURN(DEVICE_ERROR);
            if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != SUCCESS)
                RETURN(TIMEOUT);
            for (INT I = 0; I < 256; ++I)
                IdentBuffer[I] = Inw(Drive->BasePort + PATA_DATA);
            Drive->Type = PATA_TYPE_ATAPI;
            Drive->SectorSize = 2048;
            Drive->TotalSectors = 0;
            return SUCCESS;
        } else {
            RETURN(DEVICE_ERROR);
        }
    }
    
    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS) != SUCCESS)
        RETURN(TIMEOUT);
    INT Rc = WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_TIMEOUT_LOOPS);
    if (Rc != SUCCESS)
        return Rc;
    
    for (INT I = 0; I < 256; ++I)
        IdentBuffer[I] = Inw(Drive->BasePort + PATA_DATA);
    
    if (IdentBuffer[0] == 0)
        RETURN(DEVICE_ERROR);
    
    Drive->Type = PATA_TYPE_ATA;
    
    // LBA28 (words 60-61)
    UINT32 Lba28 = ((UINT32)IdentBuffer[61] << 16) | IdentBuffer[60];
    Drive->TotalSectors = Lba28;
    
    // Checking LBA48 (word 83 bit 10)
    if (IdentBuffer[83] & (1u << 10)) {
        Drive->SupportsLba48 = 1;
        UINT64 Lba48 = IdentWordsToUINT64(IdentBuffer, 100);
        if (Lba48 != 0)
            Drive->TotalSectors = Lba48;
    } else {
        Drive->SupportsLba48 = 0;
    }
    
    Drive->SectorSize = 512;
    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Initialization (improved)
 * ============================================================================
 */
static INT PataDetectDMA(PataDrive *Drive, PciDevice *PciPata) {
    if (!PciPata || PciPata->BarSizes[4] == 0)
        RETURN(NO_OBJECT);
    
    // BAR4 contains Bus Master Registers
    UINT32 Bar4 = (UINT32)Drive->PciDev->Bars[4];
    
    if (Bar4 & 1) {
        // I/O space
        Drive->BusMasterBase = Bar4 & 0xFFFC;
    } else {
        // Memory-mapped (rare for IDE)
        Drive->BusMasterBase = 0;
        RETURN(NOT_SUPPORTED);
    }
    
    // Test DMA by resetting controller
    Outb(Drive->BusMasterBase + BMCR, BMCR_RESET);
    IoDelay(Drive->CtrlPort);
    
    // Check if reset worked
    UINT8 Test = Inb(Drive->BusMasterBase + BMCR);
    if (Test & BMCR_RESET) {
        // Reset didn't complete
        RETURN(DEVICE_ERROR);
    }
    
    // Clear status
    Outb(Drive->BusMasterBase + BMSR, BMSR_INTR | BMSR_ERROR);
    
    RETURN(SUCCESS);
}

static INT PataReadWrapper(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    
    PataDrive *Drive = (PataDrive*)Disk->DriverData;
    return PataReadSectors(Drive, Lba, Count, Buffer);
}

// Запись через DiskMgr
static INT PataWriteWrapper(Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    
    PataDrive *Drive = (PataDrive*)Disk->DriverData;
    return PataWriteSectors(Drive, Lba, Count, Buffer);
}

// Resetting cache via DiskMgr
static INT PataFlushWrapper(Disk *Disk) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    
    PataDrive *Drive = (PataDrive*)Disk->DriverData;
    return PataFlushCache(Drive);
}

static INT PataRegisterDisk(PataDrive *Drive, PataChannel Channel, UINT8 DriveNum) {
    if (!Drive) return DEVICE_INVALID;
    
    // Checking that the disk really exists
    if (Drive->TotalSectors == 0 || Drive->SectorSize == 0) {
        RETURN(DEVICE_INVALID);
    }
    
    // Calculate the index in the array
    UINT32 DiskIndex = Channel * 2 + DriveNum;
    if (DiskIndex >= 4) return DEVICE_INVALID;
    
    // We get a pointer to the disk structure
    Disk *Disk = &GPataDiskObjects[DiskIndex];
    
    // Reset the structure (just in case)
    MemSet(Disk, 0, sizeof(Disk));
    
    // --- Main info ---
    Disk->Type = DISK_TYPE_PATA;
    Disk->Index = DiskIndex;
    
    // Create a name: "IDE0", "IDE1", "IDE2", "IDE3"
    DiskMakeName(Disk);  // This function is in DiskMgr.c
    
    // --- Physical parameters ---
    Disk->SectorCount = Drive->TotalSectors;
    Disk->SectorSize = Drive->SectorSize;
    Disk->TotalSize = Drive->TotalSectors * Drive->SectorSize;
    
    // --- Driver data ---
    Disk->DriverData = Drive;  // Saving PataDrive pointer
    Disk->Initialized = TRUE;
    
    // --- Read/write ---
    Disk->Read = PataReadWrapper;
    Disk->Write = PataWriteWrapper;
    Disk->Flush = PataFlushWrapper;
    
    // --- Register in DiskMgr ---
    INT Result = DiskMgrRegisterDisk(Disk);
    if (Result != SUCCESS) {
        return Result;
    }
    
    RETURN(SUCCESS);
}

INT PataInit(PataDrive *Drive, PataChannel Channel, UINT8 DriveNum) {
    if (!Drive || DriveNum > 1)
        RETURN(DEVICE_INVALID);
    
    // Initialize drive structure
    MemSet(Drive, 0, sizeof(PataDrive));
    
    // Setup base ports
    if (Channel == PATA_CHANNEL_PRIMARY) {
        Drive->BasePort = PATA_BASE_PRIMARY;
        Drive->CtrlPort = PATA_CTRL_PRIMARY;
        PrimaryIrqDrive = Drive;
    } else {
        Drive->BasePort = PATA_BASE_SECONDARY;
        Drive->CtrlPort = PATA_CTRL_SECONDARY;
        SecondaryIrqDrive = Drive;
    }
    
    PataEnableInterrupts(Drive->CtrlPort);
    Drive->Drive = DriveNum & 1;
    Drive->Channel = Channel;
    Drive->BusMasterBase = 0;
    Drive->DmaInProgress = FALSE;
    SpinLockInit(&Drive->DriveLock);
    
    // Setup IRQ
    if (Channel == PATA_CHANNEL_PRIMARY)
        Drive->Irq = 14;
    else
        Drive->Irq = 15;
    
    // Find PCI device and enable bus mastering
    PciDevice* PciPata = PciFindClass(0x01, 0x01);
    if (PciPata) {
        Drive->Irq = PciPata->InterruptLine;
        Drive->PciDev = PciPata;
        PciEnableBusmaster(PciPata);
        
        // Detect DMA capabilities
        PataDetectDMA(Drive, PciPata);
    }
    
    // IOAPIC routing
    UINT32 Gsi, Flags;
    if (IoapicGetOverride(Drive->Irq, &Gsi, &Flags) != 0) {
        Gsi = Drive->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }
    
    UINT8 Vector = 32 + Drive->Irq;
    IoapicRedirectIrq(Gsi, Vector, ApicGetId(), Flags);
    IoapicUnmaskIrq(Gsi);
    
    // Setup IDT gate
    if (Channel == PATA_CHANNEL_PRIMARY) {
        IdtSetGate(Drive->Irq, PataPrimaryIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    } else {
        IdtSetGate(Drive->Irq, PataSecondaryIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    }
    
    // Initialize queues
    SpinLockInit(&GPrimaryQueueLock);
    SpinLockInit(&GSecondaryQueueLock);
    
    // Identify the drive
    UINT16 Ident[256];
    INT Rc = PataIdentify(Drive, Ident);
    if (Rc != 0)
        return Rc;
    
    KDriverRegister(KDriverGenerateStruct("PataDisk", DCL1, TRUE, NULLPTR, NULLPTR));
    
    if (Drive->TotalSectors > 0) {
        PataRegisterDisk(Drive, Channel, DriveNum);
    }

    RETURN(SUCCESS);
}

/*
 * ============================================================================
 * Utility Functions
 * ============================================================================
 */
UINT8 PataGetIrqCount(PataDrive *Drive) {
    return Drive ? Drive->IrqCount : 0;
}

void PataResetStatistics(PataDrive *Drive) {
    if (!Drive)
        return;
    
    Drive->TotalReads = 0;
    Drive->TotalWrites = 0;
    Drive->DmaFailures = 0;
    Drive->PioFallbacks = 0;
}
