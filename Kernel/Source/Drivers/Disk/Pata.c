#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/Pata.h>
#include <InterruptList.h>
#include <Mem/PhysAlloc.h>
#include <Lib/String.h>
#include <Mem/Paging.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
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
#define ATAPI_PACKET_TIMEOUT_MS   5000

/* ATAPI signature after IDENTIFY_PACKET */
#define ATAPI_SIG_LBA_MID   0x14
#define ATAPI_SIG_LBA_HIGH  0xEB

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
STATIC Disk GPataDiskObjects[4];       /* ATA/ATAPI disks: 0..3 */

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
 * PIO (for ATA)
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
 * ATAPI HELPERS
 * ============================================================================ */

STATIC VOID AtapiWriteCdb(UINT16 Base, const UINT8 *Cdb, UINT32 Len) {
    for (UINT32 I = 0; I + 1 < Len; I += 2) {
        UINT16 Word = (UINT16)Cdb[I] | ((UINT16)Cdb[I + 1] << 8);
        Outw(Base + PATA_DATA, Word);
    }
}

STATIC VOID AtapiReadBytes(UINT16 Base, UINT8 *Out, UINT32 ByteCount) {
    UINT32 WordCount = ByteCount / 2;
    for (UINT32 I = 0; I < WordCount; I++) {
        UINT16 W = Inw(Base + PATA_DATA);
        Out[I * 2 + 0] = (UINT8)(W & 0xFF);
        Out[I * 2 + 1] = (UINT8)((W >> 8) & 0xFF);
    }
    if (ByteCount & 1) {
        UINT16 W = Inw(Base + PATA_DATA);
        Out[ByteCount - 1] = (UINT8)(W & 0xFF);
    }
}

STATIC VOID AtapiWriteBytes(UINT16 Base, const UINT8 *In, UINT32 ByteCount) {
    UINT32 WordCount = ByteCount / 2;
    for (UINT32 I = 0; I < WordCount; I++) {
        UINT16 W = (UINT16)In[I * 2] | ((UINT16)In[I * 2 + 1] << 8);
        Outw(Base + PATA_DATA, W);
    }
    if (ByteCount & 1) {
        Outw(Base + PATA_DATA, In[ByteCount - 1]);
    }
}

INT ATTRIBUTE(noinline) PataAtapiPacketEx(PataDrive *Drive, const UINT8 *Cdb,
                             UINT32 ReadBytes, VOID *ReadBuf,
                             UINT32 WriteBytes, const VOID *WriteBuf,
                             UINT8 *SenseOut) {
    if (!Drive || !Cdb) RETURN(DEVICE_INVALID);
    if (Drive->Type != PATA_TYPE_ATAPI) RETURN(DEVICE_INVALID);
    if (ReadBytes && !ReadBuf) RETURN(DEVICE_INVALID);
    if (WriteBytes && !WriteBuf) RETURN(DEVICE_INVALID);
    if (ReadBytes && WriteBytes) RETURN(INCORRECT_VALUE);

    UINT16 Base = Drive->BasePort;
    UINT16 Ctrl = Drive->CtrlPort;

    /* 1. Wait BSY=0 */
    if (WaitBsyClear(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS)
        RETURN(TIMEOUT);

    /* 2. Choosing a device */
    SelectDeviceAndDelay(Base, Ctrl, Drive->Drive, 0, 0);

    /* 3. FEATURES = 0 (PIO) */
    Outb(Base + PATA_FEATURE, 0x00);

    /* 4. BYTE_COUNT_LOW/HIGH
     *
     * For read commands: we tell the controller how many bytes we are ready to accept.
     *   - INQUIRY: 36
     *   - READ_CAPACITY: 8
     *   - READ_10: SectorSize * Count
     *   - REQUEST_SENSE: 18
     *
     * For write commands: the exact size of the data.
     */
    UINT16 ByteCount = 0;
    if (ReadBytes)  ByteCount = (ReadBytes  > 0xFFFF) ? 0xFFFF : (UINT16)ReadBytes;
    if (WriteBytes) ByteCount = (WriteBytes > 0xFFFF) ? 0xFFFF : (UINT16)WriteBytes;

    Outb(Base + PATA_NSECT, (UINT8)((ByteCount >> 8) & 0xFF));  /* BYTE_COUNT_HIGH */
    Outb(Base + PATA_LCYL,  (UINT8)(ByteCount & 0xFF));         /* BYTE_COUNT_LOW  */
    Outb(Base + PATA_HCYL,  0);                                 /* not used  */

    /* 5. PACKET command */
    Outb(Base + PATA_COMMAND, PATA_CMD_PACKET);

    /* 6. We are waiting for the DRQ — the device is ready to accept the CDB */
    if (WaitDrqOrErr(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS) {
        if (SenseOut) PataAtapiRequestSense(Drive);
        RETURN(DEVICE_ERROR);
    }

    /* 7. Writing a 12-byte CDB */
    AtapiWriteCdb(Base, Cdb, 12);

    /* 8. Command without data — waiting for completion */
    if (ReadBytes == 0 && WriteBytes == 0) {
        if (WaitBsyClear(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(TIMEOUT);
        UINT8 Status = Inb(Base + PATA_STATUS);
        if (Status & PATA_STATUS_ERR) {
            if (SenseOut) PataAtapiRequestSense(Drive);
            RETURN(DEVICE_ERROR);
        }
        RETURN(SUCCESS);
    }

    /* 9. Read: waiting for the DRQ, reading the data */
    if (ReadBytes > 0) {
        UINT8 *Out = (UINT8 *)ReadBuf;
        UINT32 Remaining = ReadBytes;

        while (Remaining > 0) {
            UINT32 Chunk = (Remaining > 512) ? 512 : Remaining;

            if (WaitDrqOrErr(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS) {
                if (SenseOut) PataAtapiRequestSense(Drive);
                RETURN(DEVICE_ERROR);
            }

            AtapiReadBytes(Base, Out, Chunk);

            Out += Chunk;
            Remaining -= Chunk;
        }

        if (WaitBsyClear(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(TIMEOUT);

        UINT8 Status = Inb(Base + PATA_STATUS);
        if (Status & PATA_STATUS_ERR) {
            if (SenseOut) PataAtapiRequestSense(Drive);
            RETURN(DEVICE_ERROR);
        }

        CheckErrAndClear(Base);
        RETURN(SUCCESS);
    }

    /* 10. Write: waiting for DRQ, writing data */
    if (WriteBytes > 0) {
        const UINT8 *In = (const UINT8 *)WriteBuf;
        UINT32 Remaining = WriteBytes;

        while (Remaining > 0) {
            UINT32 Chunk = (Remaining > 512) ? 512 : Remaining;

            if (WaitDrqOrErr(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS) {
                if (SenseOut) PataAtapiRequestSense(Drive);
                RETURN(DEVICE_ERROR);
            }

            AtapiWriteBytes(Base, In, Chunk);

            In += Chunk;
            Remaining -= Chunk;
        }

        if (WaitBsyClear(Base, Ctrl, PATA_POLL_TIMEOUT_MS) != SUCCESS)
            RETURN(TIMEOUT);

        UINT8 Status = Inb(Base + PATA_STATUS);
        if (Status & PATA_STATUS_ERR) {
            if (SenseOut) PataAtapiRequestSense(Drive);
            RETURN(DEVICE_ERROR);
        }

        CheckErrAndClear(Base);
        RETURN(SUCCESS);
    }

    RETURN(SUCCESS);
}

/* Simplified wrapper for reading commands */
INT ATTRIBUTE(noinline) PataAtapiPacket(PataDrive *Drive, const UINT8 *Cdb,
                           UINT32 ReadBytes, VOID *ReadBuf,
                           UINT8 *SenseOut) {
    return PataAtapiPacketEx(Drive, Cdb, ReadBytes, ReadBuf,
                             0, NULLPTR, SenseOut);
}

/* ============================================================================
 * ATAPI COMMANDS
 * ============================================================================ */

INT PataAtapiRequestSense(PataDrive *Drive) {
    if (!Drive || Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);

    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_REQUEST_SENSE;
    Cdb[4] = 18;  /* Allocation Length */

    UINT8 Sense[18];
    MemSet(Sense, 0, 18);

    /* Sense-out is not needed in this command, otherwise recursion */
    INT Rc = PataAtapiPacket(Drive, Cdb, 18, Sense, NULLPTR);
    if (Rc != SUCCESS)
        RETURN(Rc);

    Drive->AtapiSenseKey = Sense[2] & 0x0F;
    Drive->AtapiAsc      = Sense[12];
    Drive->AtapiAscq     = Sense[13];

    if (Drive->AtapiSenseKey == 0x02 && Drive->AtapiAsc == 0x3A)
        Drive->AtapiMediaPresent = FALSE;
    if (Drive->AtapiSenseKey == 0x06)
        Drive->AtapiMediaPresent = FALSE;

    RETURN(SUCCESS);
}

INT PataAtapiTestUnitReady(PataDrive *Drive) {
    if (!Drive || Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);

    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_TEST_UNIT_READY;

    INT Rc = PataAtapiPacket(Drive, Cdb, 0, NULLPTR, NULLPTR);

    if (Rc == SUCCESS) {
        Drive->AtapiMediaPresent = TRUE;
        RETURN(SUCCESS);
    }

    /* We are trying to query sense to understand the reason. */
    PataAtapiRequestSense(Drive);

    /* Sense Key 0x02/ASC 0x3A → no disk. This is not a driver error.. */
    if (Drive->AtapiSenseKey == 0x02 && Drive->AtapiAsc == 0x3A) {
        Drive->AtapiMediaPresent = FALSE;
        RETURN(NO_OBJECT);
    }

    RETURN(Rc);
}

INT PataAtapiInquiry(PataDrive *Drive) {
    if (!Drive || Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);

    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_INQUIRY;
    Cdb[4] = 36;

    UINT8 Resp[36];
    MemSet(Resp, 0, 36);

    INT Rc = PataAtapiPacket(Drive, Cdb, 36, Resp, NULLPTR);
    if (Rc != SUCCESS)
        RETURN(Rc);

    MemCpy(Drive->AtapiVendor,   Resp + 8,  8);
    Drive->AtapiVendor[8] = '\0';
    MemCpy(Drive->AtapiProduct,  Resp + 16, 16);
    Drive->AtapiProduct[16] = '\0';
    MemCpy(Drive->AtapiRevision, Resp + 32, 4);
    Drive->AtapiRevision[4] = '\0';

    for (INT I = 7; I >= 0 && Drive->AtapiVendor[I] == ' '; I--)
        Drive->AtapiVendor[I] = '\0';
    for (INT I = 15; I >= 0 && Drive->AtapiProduct[I] == ' '; I--)
        Drive->AtapiProduct[I] = '\0';
    for (INT I = 3; I >= 0 && Drive->AtapiRevision[I] == ' '; I--)
        Drive->AtapiRevision[I] = '\0';

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "atapi: %s %s %s\n",
                  Drive->AtapiVendor,
                  Drive->AtapiProduct,
                  Drive->AtapiRevision);

    RETURN(SUCCESS);
}

INT PataAtapiReadCapacity(PataDrive *Drive) {
    if (!Drive || Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);

    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_READ_CAPACITY_10;

    UINT8 Resp[8];
    MemSet(Resp, 0, 8);

    INT Rc = PataAtapiPacket(Drive, Cdb, 8, Resp, NULLPTR);
    if (Rc != SUCCESS)
        RETURN(Rc);

    UINT32 LastLba = ((UINT32)Resp[0] << 24) |
                     ((UINT32)Resp[1] << 16) |
                     ((UINT32)Resp[2] << 8)  |
                     ((UINT32)Resp[3]);
    UINT32 BlockSize = ((UINT32)Resp[4] << 24) |
                       ((UINT32)Resp[5] << 16) |
                       ((UINT32)Resp[6] << 8)  |
                       ((UINT32)Resp[7]);

    if (BlockSize == 0 || BlockSize > 8192)
        BlockSize = ATAPI_SECTOR_SIZE;

    Drive->AtapiSectorSize = BlockSize;
    Drive->SectorSize = (UINT16)BlockSize;
    Drive->TotalSectors = (UINT64)LastLba + 1;

    BaseconPrintf(BASECON_TYPE_NORMAL,
                  "atapi: capacity: %llu sectors x %u bytes (%llu MB)\n",
                  Drive->TotalSectors, BlockSize,
                  (Drive->TotalSectors * BlockSize) / (1024 * 1024));

    RETURN(SUCCESS);
}

INT PataAtapiStartStopUnit(PataDrive *Drive, BOOL Start, BOOL LoEj) {
    if (!Drive || Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);

    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_START_STOP_UNIT;
    Cdb[4] = (Start ? 0x01 : 0x00) | (LoEj ? 0x02 : 0x00);

    INT Rc = PataAtapiPacket(Drive, Cdb, 0, NULLPTR, NULLPTR);
    if (Rc != SUCCESS) {
        PataAtapiRequestSense(Drive);
        RETURN(Rc);
    }
    RETURN(SUCCESS);
}

INT PataAtapiReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);
    if (Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);
    if (!Drive->AtapiMediaPresent)
        RETURN(NO_OBJECT);

    if (Lba + Count > Drive->TotalSectors)
        RETURN(INCORRECT_VALUE);

    UINT32 SectorSize = Drive->AtapiSectorSize ? Drive->AtapiSectorSize : ATAPI_SECTOR_SIZE;
    UINT8 *Out = (UINT8 *)Buffer;
    UINT64 CurLba = Lba;
    UINT32 Remaining = Count;

    while (Remaining > 0) {
        UINT16 ThisCount = (Remaining > 0xFFFF) ? 0xFFFF : (UINT16)Remaining;

        UINT8 Cdb[12];
        MemSet(Cdb, 0, 12);
        Cdb[0] = SCSI_READ_10;
        Cdb[1] = 0x00;
        Cdb[2] = (UINT8)((CurLba >> 24) & 0xFF);
        Cdb[3] = (UINT8)((CurLba >> 16) & 0xFF);
        Cdb[4] = (UINT8)((CurLba >>  8) & 0xFF);
        Cdb[5] = (UINT8)((CurLba      ) & 0xFF);
        Cdb[6] = 0x00;
        Cdb[7] = (UINT8)((ThisCount >> 8) & 0xFF);
        Cdb[8] = (UINT8)((ThisCount     ) & 0xFF);
        Cdb[9] = 0x00;

        UINT32 BytesThis = (UINT32)ThisCount * SectorSize;

        INT Rc = PataAtapiPacket(Drive, Cdb, BytesThis, Out, NULLPTR);
        if (Rc != SUCCESS) {
            PataAtapiRequestSense(Drive);
            /* One repeat attempt */
            Rc = PataAtapiPacket(Drive, Cdb, BytesThis, Out, NULLPTR);
            if (Rc != SUCCESS)
                RETURN(Rc);
        }

        Out += BytesThis;
        CurLba += ThisCount;
        Remaining -= ThisCount;
    }

    Drive->TotalReads++;
    RETURN(SUCCESS);
}

/* ============================================================================
 * DMA (for ATA)
 * ============================================================================ */

STATIC INT PataDmaTransfer(PataDrive *Drive, PataRequest *Req) {
    if (!Drive || !Req || !Drive->BusMasterBase || Req->Count == 0)
        RETURN(DEVICE_INVALID);

    if (Drive->Type == PATA_TYPE_ATAPI)
        RETURN(NOT_SUPPORTED);

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
        UINT8 BmsrStatus = Inb(Drive->BusMasterBase + BMSR);

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

    /* ATAPI — always synchronously via PIO PACKET */
    if (Drive->Type == PATA_TYPE_ATAPI) {
        Req->Result = PataAtapiReadSectors(Drive, Lba, Count, Buffer);
        Req->Completed = TRUE;
        return Req->Result;
    }

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

    if (Drive->Type == PATA_TYPE_ATAPI)
        RETURN(NOT_SUPPORTED);

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
        /* sti; pause; cli — so that IRQ can set Completed */
        __asm__ volatile ("sti; pause; cli" ::: "memory");
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

    if (Drive->Type == PATA_TYPE_ATAPI)
        return PataAtapiReadSectors(Drive, Lba, Count, Buffer);

    PataRequest Req;
    INT Result = PataReadSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;

    return PataWaitRequest(&Req, PATA_DMA_TIMEOUT_MS);
}

INT PataWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Drive || !Buffer || Count == 0)
        RETURN(DEVICE_INVALID);

    if (Drive->Type == PATA_TYPE_ATAPI)
        RETURN(NOT_SUPPORTED);

    PataRequest Req;
    INT Result = PataWriteSectorsAsync(Drive, Lba, Count, Buffer, &Req);
    if (Result != 0)
        return Result;

    return PataWaitRequest(&Req, PATA_DMA_TIMEOUT_MS);
}

INT PataFlushCache(PataDrive *Drive) {
    if (!Drive)
        return -DEVICE_INVALID;

    if (Drive->Type == PATA_TYPE_ATAPI)
        return SUCCESS;   /* The CD does not have a write cache */

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
 * IDENTIFY (ATA + ATAPI)
 * ============================================================================ */

INT PataIdentify(PataDrive *Drive, UINT16 IdentBuffer[256]) {
    if (!Drive || !IdentBuffer)
        RETURN(DEVICE_INVALID);

    Drive->Type = PATA_TYPE_NONE;
    Drive->IsAtapi = FALSE;
    Drive->SupportsLba48 = 0;
    Drive->SectorSize = 512;
    Drive->TotalSectors = 0;
    Drive->AtapiSectorSize = 0;
    Drive->AtapiMediaPresent = FALSE;
    Drive->IrqPending = 0;
    Drive->IrqCount = 0;
    Drive->Model[0] = '\0';

    /* Step 1: Try IDENTIFY (ATA) */
    SelectDeviceAndDelay(Drive->BasePort, Drive->CtrlPort, Drive->Drive, 0, 0);

    Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY);
    IoDelay(Drive->CtrlPort);

    UINT8 Status = Inb(Drive->BasePort + PATA_STATUS);
    if (Status == 0)
        RETURN(DEVICE_ERROR);   /* There is no device */

    /* Step 2: Check the ATAPI signature */
    if (Status & PATA_STATUS_ERR) {
        UINT8 Cl = Inb(Drive->BasePort + PATA_LCYL);   /* LBA_MID  */
        UINT8 Ch = Inb(Drive->BasePort + PATA_HCYL);   /* LBA_HIGH */

        if ((Cl == ATAPI_SIG_LBA_MID && Ch == ATAPI_SIG_LBA_HIGH) ||
            (Cl == 0x69 && Ch == 0x96)) {

            Drive->Type = PATA_TYPE_ATAPI;
            Drive->IsAtapi = TRUE;
            Drive->SectorSize = ATAPI_SECTOR_SIZE;
            Drive->AtapiSectorSize = ATAPI_SECTOR_SIZE;

            /* Resetting the error */
            Inb(Drive->BasePort + PATA_ERROR);

            SelectDeviceAndDelay(Drive->BasePort, Drive->CtrlPort,
                                 Drive->Drive, 0, 0);

            Outb(Drive->BasePort + PATA_COMMAND, PATA_CMD_IDENTIFY_PACKET);
            IoDelay(Drive->CtrlPort);

            if (WaitBsyClear(Drive->BasePort, Drive->CtrlPort,
                             PATA_POLL_TIMEOUT_MS) != SUCCESS)
                RETURN(TIMEOUT);

            INT Rc = WaitDrqOrErr(Drive->BasePort, Drive->CtrlPort,
                                  PATA_POLL_TIMEOUT_MS);
            if (Rc != SUCCESS)
                return Rc;

            for (INT I = 0; I < 256; ++I)
                IdentBuffer[I] = Inw(Drive->BasePort + PATA_DATA);

            /* Model (words 27-46) */
            for (INT I = 0; I < 20; I++) {
                UINT16 W = IdentBuffer[27 + I];
                Drive->Model[I * 2 + 0] = (CHAR)(W >> 8);
                Drive->Model[I * 2 + 1] = (CHAR)(W & 0xFF);
            }
            Drive->Model[40] = '\0';
            for (INT I = 39; I >= 0 &&
                 (Drive->Model[I] == ' ' || Drive->Model[I] == '\0'); I--)
                Drive->Model[I] = '\0';

            /* ATAPI-specific queries */
            PataAtapiInquiry(Drive);

            INT MediaRc = PataAtapiTestUnitReady(Drive);
            if (MediaRc == SUCCESS) {
                PataAtapiReadCapacity(Drive);
            } else {
                BaseconPrintf(BASECON_TYPE_NORMAL,
                              "atapi: no media in drive %u\n",
                              Drive->Drive);
            }

            RETURN(SUCCESS);
        }

        RETURN(DEVICE_ERROR);
    }

    /* Step 3: Regular ATA */
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
    Drive->IsAtapi = FALSE;

    UINT32 Lba28 = ((UINT32)IdentBuffer[61] << 16) | IdentBuffer[60];
    Drive->TotalSectors = Lba28;

    if (IdentBuffer[83] & (1u << 10)) {
        Drive->SupportsLba48 = 1;
        UINT64 Lba48 = IdentWordsToUINT64(IdentBuffer, 100);
        if (Lba48 != 0)
            Drive->TotalSectors = Lba48;
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
 * DMA DETECTION
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

/* ============================================================================
 * DISK WRAPPERS
 * ============================================================================ */

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

    Disk *PDisk = &GPataDiskObjects[DiskIndex];
    MemSet(PDisk, 0, sizeof(Disk));

    if (Drive->Type == PATA_TYPE_ATAPI) {
        PDisk->Type = DISK_TYPE_OPTICAL;
    } else {
        PDisk->Type = DISK_TYPE_PATA;
    }
    PDisk->Index = DiskIndex;
    DiskMakeName(PDisk);

    PDisk->SectorCount = Drive->TotalSectors;
    PDisk->SectorSize = Drive->SectorSize;
    PDisk->TotalSize = PDisk->SectorCount * PDisk->SectorSize;
    PDisk->DriverData = Drive;
    PDisk->Initialized = TRUE;
    PDisk->Read = PataReadWrapper;

    SnPrintf(PDisk->Name, sizeof(PDisk->Name),
             Drive->IsAtapi ? "PATAPI%d" : "PATA%d", DiskIndex);

    if (Drive->Type == PATA_TYPE_ATAPI) {
        PDisk->Write = NULLPTR;   /* CD-ROM is read-only */
    } else {
        PDisk->Write = PataWriteWrapper;
    }
    PDisk->Flush = PataFlushWrapper;

    INT Result = DiskMgrRegisterDisk(PDisk);
    if (Result != SUCCESS) {
        return Result;
    }

    RETURN(SUCCESS);
}
	
/* ============================================================================
 * INIT
 * ============================================================================ */

EXTERN(VOID, PataPrimaryIrq());
EXTERN(VOID, PataSecondaryIrq());

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
        IdtSetGate(Vector, PataPrimaryIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    } else {
        IdtSetGate(Vector, PataSecondaryIrq, KERNEL_CODE_SEL, IDT_GATE_INT, 0);
    }

    SpinLockInit(&GPrimaryQueueLock);
    SpinLockInit(&GSecondaryQueueLock);

    UINT16 Ident[256];
    INT Rc = PataIdentify(Drive, Ident);
    if (Rc == NOT_SUPPORTED) {
        RETURN(NOT_SUPPORTED);
    }
    if (Rc != 0)
        return Rc;

    KDriverRegister(KDriverGenerateStruct("PataDisk", DCL1, TRUE, NULLPTR, NULLPTR));

    if (Drive->TotalSectors > 0) {
        PataRegisterDisk(Drive, Channel, DriveNum);
    } else if (Drive->Type == PATA_TYPE_ATAPI) {
        /* ATAPI without a disk — we register it with a zero size.
         * The user will insert the disc and re-scan it. */
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

/* ============================================================================
 * Public wrapper for ATAPI rescanning (after inserting the disc)
 * ============================================================================ */

INT PataAtapiRescan(PataDrive *Drive) {
    if (!Drive || Drive->Type != PATA_TYPE_ATAPI)
        RETURN(DEVICE_INVALID);

    INT Rc = PataAtapiTestUnitReady(Drive);
    if (Rc == SUCCESS) {
        PataAtapiReadCapacity(Drive);
    }
    RETURN(Rc);
}