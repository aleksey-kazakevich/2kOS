#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/Pata.h>
#include <InterruptList.h>
#include <Mem/PhysAlloc.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <Time/Timer.h>
#include <SysDevice.h>
#include <SpinLock.h>
#include <Basecon.h>
#include <KDriver.h>
#include <Asm/Io.h>
#include <Return.h>
#include <Types.h>
#include <Idt.h>

/* ============================================================================
 * TIMEOUTS
 * ============================================================================ */

#define PATA_POLL_TIMEOUT_MS      1000

/* ============================================================================
 * GLOBAL STATE
 * ============================================================================ */

STATIC volatile PataDrive *PrimaryIrqDrive = NULLPTR;
STATIC volatile PataDrive *SecondaryIrqDrive = NULLPTR;

STATIC PataRequest *PrimaryRequestQueue = NULLPTR;
STATIC PataRequest *PrimaryRequestTail = NULLPTR;
STATIC PataRequest *SecondaryRequestQueue = NULLPTR;
STATIC PataRequest *SecondaryRequestTail = NULLPTR;

STATIC SpinLock GPrimaryQueueLock;
STATIC SpinLock GSecondaryQueueLock;

STATIC UINT32 GDmaFailures = 0;
STATIC UINT32 GPioFallbacks = 0;

PataDrive PataDrives[4];
STATIC Disk GPataDiskObjects[4];

extern ListHead GDiskList;
extern UINT32 GDiskCount;

/* ============================================================================
 * LOW-LEVEL HELPERS
 * ============================================================================ */

STATIC INLINE VOID IoDelay(UINT16 CtrlPort) {
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

STATIC INT WaitBsyClear(UINT16 BasePort, UINT16 CtrlPort, UINT32 TimeoutMs) {
    UINT64 Start = TimerTicks();
    UINT64 TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    if (TimeoutTicks == 0) TimeoutTicks = 1;

    while (1) {
        UINT8 S = Inb(BasePort + PATA_STATUS);
        if (!(S & PATA_STATUS_BSY))
            return SUCCESS;
        if ((TimerTicks() - Start) >= TimeoutTicks)
            return TIMEOUT;
        CpuPause();
    }
    (VOID)CtrlPort;
}

STATIC INT WaitDrqOrErr(UINT16 BasePort, UINT16 CtrlPort, UINT32 TimeoutMs) {
    UINT64 Start = TimerTicks();
    UINT64 TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;
    if (TimeoutTicks == 0) TimeoutTicks = 1;

    while (1) {
        UINT8 S = Inb(BasePort + PATA_STATUS);
        if (S & PATA_STATUS_ERR)
            return DEVICE_ERROR;
        if (!(S & PATA_STATUS_BSY) && (S & PATA_STATUS_DRQ))
            return SUCCESS;
        if ((TimerTicks() - Start) >= TimeoutTicks)
            return TIMEOUT;
        CpuPause();
    }
    (VOID)CtrlPort;
}

STATIC INT CheckErrAndClear(UINT16 BasePort) {
    UINT8 S = Inb(BasePort + PATA_STATUS);
    if (S & PATA_STATUS_ERR) {
        Inb(BasePort + PATA_ERROR);
        return DEVICE_ERROR;
    }
    return SUCCESS;
}

STATIC VOID SelectDeviceAndDelay(UINT16 Base, UINT16 Ctrl,
                                 UINT8 Drive, INT LbaFlag, UINT8 HeadHigh4) {
    UINT8 Value = (LbaFlag ? 0xE0 : 0xA0) | ((Drive & 1) << 4) | (HeadHigh4 & 0x0F);
    Outb(Base + PATA_SELECT, Value);
    IoDelay(Ctrl);
}

STATIC UINT64 IdentWordsToUINT64(const UINT16 Ident[256], INT W) {
    UINT64 V = 0;
    V |= (UINT64)Ident[W + 0];
    V |= (UINT64)Ident[W + 1] << 16;
    V |= (UINT64)Ident[W + 2] << 32;
    V |= (UINT64)Ident[W + 3] << 48;
    return V;
}

STATIC INLINE INT IsAligned2(const VOID *Ptr) {
    return (((UINTPTR)Ptr) & 1u) == 0;
}

STATIC VOID ReadSectorWordsTo(UINT16 Base, UINT16 AlignedWords[256]) {
    for (INT I = 0; I < 256; ++I)
        AlignedWords[I] = Inw(Base + PATA_DATA);
}

/* ============================================================================
 * PRDT MANAGEMENT
 * ============================================================================ */

STATIC PrdtEntry *PataAllocPrdt(VOID *PhysBuffer, UINT32 SectorCount, UINT16 *PrdtPhys) {
    UINT32 EntriesNeeded = (SectorCount + 62) / 63;
    if (EntriesNeeded > PRDT_ENTRY_COUNT || EntriesNeeded == 0)
        return NULLPTR;

    PrdtEntry *Prdt = (PrdtEntry *)PhysAllocAllocatePage(PhysAllocGet());
    if (!Prdt)
        return NULLPTR;

    MemSet(Prdt, 0, 4096);

    UINTPTR PhysAddrPrdt = (UINTPTR)VirtToPhysPtr(Prdt);

    if (PhysAddrPrdt > 0xFFFFFFFF || (PhysAddrPrdt & 3)) {
        PhysAllocFreePage(PhysAllocGet(), Prdt);
        return NULLPTR;
    }

    *PrdtPhys = (UINT16)PhysAddrPrdt;

    UINT32 RemainingSectors = SectorCount;
    UINT32 CurrentSector = 0;
    UINT32 EntryIndex = 0;

    while (RemainingSectors > 0 && EntryIndex < PRDT_ENTRY_COUNT) {
        UINT32 SectorsThisEntry = MIN(RemainingSectors, 63);
        UINT32 BytesThisEntry = SectorsThisEntry * 512;

        UINTPTR PhysAddr = (UINTPTR)VirtToPhysPtr((UINT8 *)PhysBuffer + CurrentSector * 512);

        UINT32 BytesToBoundary = 0x10000 - (PhysAddr & 0xFFFF);
        if (BytesThisEntry > BytesToBoundary) {
            BytesThisEntry = BytesToBoundary;
            SectorsThisEntry = BytesThisEntry / 512;
            if (SectorsThisEntry == 0) {
                PhysAllocFreePage(PhysAllocGet(), Prdt);
                return NULLPTR;
            }
        }

        Prdt[EntryIndex].PhysAddr = (UINT32)PhysAddr;
        Prdt[EntryIndex].ByteCount = (UINT16)(BytesThisEntry - 1);
        Prdt[EntryIndex].EndOfTable = (RemainingSectors <= SectorsThisEntry) ? PRDT_EOT : 0;

        RemainingSectors -= SectorsThisEntry;
        CurrentSector += SectorsThisEntry;
        EntryIndex++;
    }

    if (RemainingSectors > 0) {
        PhysAllocFreePage(PhysAllocGet(), Prdt);
        return NULLPTR;
    }

    return Prdt;
}

STATIC VOID PataFreePrdt(PrdtEntry *Prdt) {
    if (Prdt) {
        PhysAllocFreePage(PhysAllocGet(), Prdt);
    }
}

/* ============================================================================
 * LBA REGISTERS
 * ============================================================================ */

STATIC VOID SetupLba28Regs(UINT16 Base, UINT16 Ctrl, UINT32 Lba,
                           UINT8 Count, UINT8 Drive) {
    SelectDeviceAndDelay(Base, Ctrl, Drive, 1, (UINT8)((Lba >> 24) & 0x0F));
    Outb(Base + PATA_NSECT, Count);
    Outb(Base + PATA_SECTOR, (UINT8)(Lba & 0xFF));
    Outb(Base + PATA_LCYL, (UINT8)((Lba >> 8) & 0xFF));
    Outb(Base + PATA_HCYL, (UINT8)((Lba >> 16) & 0xFF));
}

STATIC VOID SetupLba48Regs(UINT16 Base, UINT16 Ctrl, UINT64 Lba,
                           UINT16 Count, UINT8 Drive) {
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

/* ============================================================================
 * PIO
 * ============================================================================ */

STATIC INT PataPioReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);

    UINT8 *UserBuf = (UINT8 *)Buffer;
    UINT16 TmpSectorWords[256];
    UINT32 Remaining = Count;

    while (Remaining > 0) {
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

        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(TIMEOUT);

        if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(DEVICE_ERROR);

        UINT16 *Dest = (UINT16 *)(UserBuf + (Count - Remaining) * 512);
        if (IsAligned2(Dest)) {
            for (INT I = 0; I < 256; I++)
                Dest[I] = Inw(Drive->BasePort + PATA_DATA);
        } else {
            ReadSectorWordsTo(Drive->BasePort, TmpSectorWords);
            MemCpy(Dest, TmpSectorWords, 512);
        }

        if (CheckErrAndClear(Drive->BasePort) != SUCCESS)
            RETURN(DEVICE_ERROR);

        Lba++;
        Remaining--;
    }

    Drive->TotalReads++;
    RETURN(SUCCESS);
}

STATIC INT PataPioWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);

    const UINT8 *UserBuf = (const UINT8 *)Buffer;
    UINT16 TmpSectorWords[256];
    UINT32 Remaining = Count;

    while (Remaining > 0) {
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

        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(TIMEOUT);

        if (WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(DEVICE_ERROR);

        const UINT16 *Src = (const UINT16 *)(UserBuf + (Count - Remaining) * 512);
        if (IsAligned2(Src)) {
            for (INT I = 0; I < 256; I++)
                Outw(Drive->BasePort + PATA_DATA, Src[I]);
        } else {
            MemCpy(TmpSectorWords, Src, 512);
            for (INT I = 0; I < 256; I++)
                Outw(Drive->BasePort + PATA_DATA, TmpSectorWords[I]);
        }

        if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(TIMEOUT);

        if (CheckErrAndClear(Drive->BasePort) != SUCCESS)
            RETURN(DEVICE_ERROR);

        Lba++;
        Remaining--;
    }

    Drive->TotalWrites++;
    RETURN(SUCCESS);
}

/* ============================================================================
 * DMA
 * ============================================================================ */

STATIC INT PataDmaTransfer(PataDrive *Drive, PataRequest *Req) {
    if (!Drive || !Req || !Drive->BusMasterBase || Req->Count == 0)
        RETURN(DEVICE_INVALID);

    if (Req->Count > MAX_DMA_SECTORS)
        RETURN(INCORRECT_VALUE);

    Req->Prdt = PataAllocPrdt(Req->Buffer, Req->Count, &Req->PrdtPhys);
    if (!Req->Prdt)
        RETURN(NO_MEMORY);

    Outb(Drive->BusMasterBase + BMCR, BMCR_RESET);
    IoDelay(Drive->CtrlPort);

    for (INT I = 0; I < 1000; I++) {
        if (!(Inb(Drive->BusMasterBase + BMCR) & BMCR_RESET))
            break;
        IoDelay(Drive->CtrlPort);
    }

    Outb(Drive->BusMasterBase + BMSR, BMSR_INTR | BMSR_ERROR);
    Outl(Drive->BusMasterBase + BMIDETBL, Req->PrdtPhys);

    BOOL UseLba48 = Drive->SupportsLba48 && (Req->Lba > 0x0FFFFFFF || Req->Count > 255);
    UINT8 Command;

    if (UseLba48) {
        SetupLba48Regs(Drive->BasePort, Drive->CtrlPort, Req->Lba,
                       (UINT16)Req->Count, Drive->Drive);
        Command = (Req->Op == PATA_OP_READ) ? PATA_CMD_READ_DMA_EXT : PATA_CMD_WRITE_DMA_EXT;
    } else {
        SetupLba28Regs(Drive->BasePort, Drive->CtrlPort, (UINT32)Req->Lba,
                       (UINT8)Req->Count, Drive->Drive);
        Command = (Req->Op == PATA_OP_READ) ? PATA_CMD_READ_DMA : PATA_CMD_WRITE_DMA;
    }

    UINT8 Bmcr = (Req->Op == PATA_OP_READ) ? BMCR_READ : 0;
    Outb(Drive->BusMasterBase + BMCR, Bmcr);

    Outb(Drive->BasePort + PATA_COMMAND, Command);
    IoDelay(Drive->CtrlPort);

    for (INT I = 0; I < PATA_TIMEOUT_LOOPS; I++) {
        UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
        UINT8 BmsrStatus = Inb(Drive->BusMasterBase + BMSR);

        (VOID)Status;

        if (BmsrStatus & BMSR_ERROR) {
            PataFreePrdt(Req->Prdt);
            Req->Prdt = NULLPTR;
            RETURN(DEVICE_ERROR);
        }

        if (BmsrStatus & BMSR_DRQ) {
            Outb(Drive->BusMasterBase + BMCR, Bmcr | BMCR_START);
            Req->DmaActive = TRUE;
            Drive->DmaInProgress = TRUE;
            RETURN(SUCCESS);
        }

        if ((I & 0xFF) == 0)
            IoDelay(Drive->CtrlPort);
    }

    PataFreePrdt(Req->Prdt);
    Req->Prdt = NULLPTR;
    RETURN(TIMEOUT);
}

/* ============================================================================
 * QUEUES
 * ============================================================================ */

STATIC VOID PataQueueRequest(PataDrive *Drive, PataRequest *Req) {
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

STATIC PataRequest *PataDequeueRequest(PataDrive *Drive) {
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

/* ============================================================================
 * IRQ COMPLETION
 * ============================================================================ */

STATIC VOID PataCompleteIrqRequest(PataDrive *Drive) {
    if (!Drive)
        return;

    PataRequest *Req = PataDequeueRequest(Drive);
    if (!Req)
        return;

    UINT8 Bmsr = Inb(Drive->BusMasterBase + BMSR);
    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);

    Outb(Drive->BusMasterBase + BMCR, 0);
    Drive->DmaInProgress = FALSE;

    if (Req->DmaActive) {
        if (Req->Result == CANCELED) {
            PataFreePrdt(Req->Prdt);
            Req->Prdt = NULLPTR;
            Req->DmaActive = FALSE;
        } else {
            if ((Bmsr & BMSR_ERROR) || (Status & PATA_STATUS_ERR)) {
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

        PataFreePrdt(Req->Prdt);
        Req->Prdt = NULLPTR;
        Req->DmaActive = FALSE;
    } else {
        Req->Result = 0;
    }

    Outb(Drive->BusMasterBase + BMSR, Bmsr | BMSR_INTR | BMSR_ERROR);
    Req->Completed = TRUE;
}

/* ============================================================================
 * IRQ HANDLERS
 * ============================================================================ */

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

/* ============================================================================
 * ASYNC API
 * ============================================================================ */

INT PataReadSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count,
                         VOID *Buffer, PataRequest *Req) {
    if (!Drive || !Buffer || !Req || Count == 0)
        RETURN(DEVICE_INVALID);

    if (Drive->TotalSectors && Lba + Count > Drive->TotalSectors)
        RETURN(DEVICE_INVALID);

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

    INT Result = PataDmaTransfer(Drive, Req);
    if (Result != 0) {
        Req->Result = PataPioReadSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }

    PataQueueRequest(Drive, Req);
    RETURN(SUCCESS);
}

INT PataWriteSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count,
                          const VOID *Buffer, PataRequest *Req) {
    if (!Drive || !Buffer || !Req || Count == 0)
        RETURN(DEVICE_INVALID);

    if (Drive->TotalSectors && Lba + Count > Drive->TotalSectors)
        RETURN(DEVICE_INVALID);

    MemSet(Req, 0, sizeof(PataRequest));
    Req->Op = PATA_OP_WRITE;
    Req->Lba = Lba;
    Req->Count = Count;
    Req->Buffer = (VOID *)Buffer;
    Req->Completed = FALSE;
    Req->DmaActive = FALSE;
    Req->Result = -1;
    Req->Prdt = NULLPTR;
    SpinLockInit(&Req->Lock);

    INT Result = PataDmaTransfer(Drive, Req);
    if (Result != 0) {
        Req->Result = PataPioWriteSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }

    PataQueueRequest(Drive, Req);
    RETURN(SUCCESS);
}

/* ============================================================================
 * WAIT / CANCEL
 * ============================================================================ */

INT PataWaitRequest(PataRequest *Req, UINT32 TimeoutMs) {
    if (!Req)
        RETURN(DEVICE_INVALID);

    if (Req->Completed)
        return Req->Result;

    UINT64 StartTicks = TimerTicks();
    UINT64 TimeoutTicks = (UINT64)TimeoutMs * TimerFreq() / 1000;

    while (!Req->Completed) {
        if (TimeoutMs > 0 && (TimerTicks() - StartTicks) >= TimeoutTicks) {
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

    if (Req->DmaActive) {
        Req->Result = CANCELED;
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

/* ============================================================================
 * SYNC API
 * ============================================================================ */

INT PataReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);

    PataRequest Req;
    INT Result = PataReadSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;

    return PataWaitRequest(&Req, PATA_DMA_TIMEOUT_MS);
}

INT PataWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);

    PataRequest Req;
    INT Result = PataWriteSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;

    return PataWaitRequest(&Req, PATA_DMA_TIMEOUT_MS);
}

INT PataFlushCache(PataDrive *Drive) {
    if (!Drive)
        return -DEVICE_INVALID;

    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
        return -TIMEOUT;

    if (Drive->SupportsLba48) {
        Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_CACHE_FLUSH_EXT);
    } else {
        Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_CACHE_FLUSH);
    }

    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort, PATA_POLL_TIMEOUT_MS) != SUCCESS)
        return -TIMEOUT;

    return CheckErrAndClear(Drive->BasePort);
}

/* ============================================================================
 * IDENTIFY (ATA only)
 * ============================================================================ */

INT PataIdentify(PataDrive *Drive, UINT16 IdentBuffer[256]) {
    if (!Drive || !IdentBuffer)
        RETURN(DEVICE_INVALID);

    Drive->Type = PATA_TYPE_NONE;
    Drive->SupportsLba48 = 0;
    Drive->SectorSize = 512;
    Drive->TotalSectors = 0;
    Drive->IrqPending = 0;
    Drive->IrqCount = 0;
    Drive->Model[0] = '\0';

    SelectDeviceAndDelay(Drive->BasePort, Drive->CtrlPort, Drive->Drive, 0, 0);

    Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY);
    IoDelay(Drive->CtrlPort);

    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
    if (Status == 0)
        RETURN(DEVICE_ERROR);

    /* Если ERR и сигнатура ATAPI — не наш диск, скипаем */
    if (Status & PATA_STATUS_ERR) {
        UINT8 Cl = Inb(Drive->BasePort + PATA_LCYL);
        UINT8 Ch = Inb(Drive->BasePort + PATA_HCYL);

        if ((Cl == 0x14 && Ch == 0xEB) || (Cl == 0x69 && Ch == 0x96)) {
            /* ATAPI — не поддерживаем, выходим */
            RETURN(NOT_SUPPORTED);
        }
        RETURN(DEVICE_ERROR);
    }

    if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort,
                     PATA_POLL_TIMEOUT_MS) != SUCCESS)
        RETURN(TIMEOUT);

    INT Rc = WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort,
                          PATA_POLL_TIMEOUT_MS);
    if (Rc != SUCCESS)
        return Rc;

    for (INT I = 0; I < 256; ++I)
        IdentBuffer[I] = Inw(Drive->BasePort + PATA_DATA);

    if (IdentBuffer[0] == 0)
        RETURN(DEVICE_ERROR);

    Drive->Type = PATA_TYPE_ATA;

    UINT32 Lba28 = ((UINT32)IdentBuffer[61] << 16) | IdentBuffer[60];
    Drive->TotalSectors = Lba28;

    if (IdentBuffer[83] & (1u << 10)) {
        Drive->SupportsLba48 = 1;
        UINT64 Lba48 = IdentWordsToUINT64(IdentBuffer, 100);
        if (Lba48 != 0)
            Drive->TotalSectors = Lba48;
    } else {
        Drive->SupportsLba48 = 0;
    }

    Drive->SectorSize = 512;

    for (INT I = 0; I < 20; I++) {
        UINT16 W = IdentBuffer[27 + I];
        Drive->Model[I * 2 + 0] = (CHAR)(W >> 8);
        Drive->Model[I * 2 + 1] = (CHAR)(W & 0xFF);
    }
    Drive->Model[40] = '\0';
    for (INT I = 39; I >= 0 &&
         (Drive->Model[I] == ' ' || Drive->Model[I] == '\0'); I--)
        Drive->Model[I] = '\0';

    RETURN(SUCCESS);
}

/* ============================================================================
 * DISK REGISTRATION
 * ============================================================================ */

STATIC INT PataDetectDMA(PataDrive *Drive, PciDevice *PciPata) {
    if (!PciPata || PciPata->BarSizes[4] == 0)
        RETURN(NO_OBJECT);

    UINT32 Bar4 = (UINT32)Drive->PciDev->Bars[4];

    if (Bar4 & 1) {
        Drive->BusMasterBase = Bar4 & 0xFFFC;
    } else {
        Drive->BusMasterBase = 0;
        RETURN(NOT_SUPPORTED);
    }

    Outb(Drive->BusMasterBase + BMCR, BMCR_RESET);
    IoDelay(Drive->CtrlPort);

    UINT8 Test = Inb(Drive->BusMasterBase + BMCR);
    if (Test & BMCR_RESET) {
        RETURN(DEVICE_ERROR);
    }

    Outb(Drive->BusMasterBase + BMSR, BMSR_INTR | BMSR_ERROR);

    RETURN(SUCCESS);
}

STATIC INT PataReadWrapper(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    PataDrive *Drive = (PataDrive *)Disk->DriverData;
    return PataReadSectors(Drive, Lba, Count, Buffer);
}

STATIC INT PataWriteWrapper(Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    PataDrive *Drive = (PataDrive *)Disk->DriverData;
    return PataWriteSectors(Drive, Lba, Count, Buffer);
}

STATIC INT PataFlushWrapper(Disk *Disk) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    PataDrive *Drive = (PataDrive *)Disk->DriverData;
    return PataFlushCache(Drive);
}

STATIC INT PataRegisterDisk(PataDrive *Drive, PataChannel Channel, UINT8 DriveNum) {
    if (!Drive) return DEVICE_INVALID;
    if (Drive->TotalSectors == 0 || Drive->SectorSize == 0)
        RETURN(DEVICE_INVALID);

    UINT32 DiskIndex = Channel * 2 + DriveNum;
    if (DiskIndex >= 4) return DEVICE_INVALID;

    Disk *Disk = &GPataDiskObjects[DiskIndex];
    MemSet(Disk, 0, sizeof(Disk));

    Disk->Type = DISK_TYPE_PATA;
    Disk->Index = DiskIndex;
    DiskMakeName(Disk);

    Disk->SectorCount = Drive->TotalSectors;
    Disk->SectorSize = Drive->SectorSize;
    Disk->TotalSize = Disk->SectorCount * Disk->SectorSize;
    Disk->DriverData = Drive;
    Disk->Initialized = TRUE;
    Disk->Read = PataReadWrapper;
    Disk->Write = PataWriteWrapper;
    Disk->Flush = PataFlushWrapper;

    INT Result = DiskMgrRegisterDisk(Disk);
    if (Result != SUCCESS) {
        return Result;
    }

    RETURN(SUCCESS);
}

/* ============================================================================
 * INIT
 * ============================================================================ */

INT PataInit(PataDrive *Drive, PataChannel Channel, UINT8 DriveNum) {
    if (!Drive || DriveNum > 1)
        RETURN(DEVICE_INVALID);

    MemSet(Drive, 0, sizeof(PataDrive));

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

    Drive->Irq = (Channel == PATA_CHANNEL_PRIMARY) ? 14 : 15;

    PciDevice *PciPata = PciFindClass(0x01, 0x01);
    if (PciPata) {
        Drive->Irq = PciPata->InterruptLine;
        Drive->PciDev = PciPata;
        PciEnableBusmaster(PciPata);
        PataDetectDMA(Drive, PciPata);
    }

    UINT32 Gsi, Flags;
    if (IoapicGetOverride(Drive->Irq, &Gsi, &Flags) != 0) {
        Gsi = Drive->Irq;
        Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;
    }

    UINT8 Vector = 32 + Drive->Irq;
    IoapicRedirectIrq(Gsi, Vector, ApicGetId(), Flags);
    IoapicUnmaskIrq(Gsi);

    if (Channel == PATA_CHANNEL_PRIMARY) {
        IdtSetGate(Drive->Irq, PataPrimaryIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    } else {
        IdtSetGate(Drive->Irq, PataSecondaryIrqHandler, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    }

    SpinLockInit(&GPrimaryQueueLock);
    SpinLockInit(&GSecondaryQueueLock);

    UINT16 Ident[256];
    INT Rc = PataIdentify(Drive, Ident);
    if (Rc == NOT_SUPPORTED) {
        /* ATAPI — скипаем тихо */
        RETURN(NOT_SUPPORTED);
    }
    if (Rc != 0)
        return Rc;

    KDriverRegister(KDriverGenerateStruct("PataDisk", DCL1, TRUE, NULLPTR, NULLPTR));

    if (Drive->TotalSectors > 0) {
        PataRegisterDisk(Drive, Channel, DriveNum);
    }

    RETURN(SUCCESS);
}

/* ============================================================================
 * UTILITY
 * ============================================================================ */

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