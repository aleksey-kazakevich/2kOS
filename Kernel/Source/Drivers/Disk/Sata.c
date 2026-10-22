#include <Drivers/Irq/Apic/Ioapic.h>
#include <Drivers/Irq/Apic/Apic.h>
#include <Drivers/Disk/DiskMgr.h>
#include <Drivers/Disk/Sata.h>
#include <Mem/PhysAlloc.h>
#include <Mem/Allocator.h>
#include <Mem/Paging.h>
#include <Drivers/Pci.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
#include <KDriver.h>
#include <Basecon.h>
#include <Asm/Cpu.h>
#include <Return.h>
#include <Idt.h>

// ==================== IRQ CONFIGURATION ====================
#define SATA_IRQ_VECTOR     0x42
#define SATA_IRQ_TIMEOUT_MS 5000
#define SATA_DEBUG          1

// ==================== GLOBALS ====================
STATIC HbaMem* GSataHba = NULLPTR;
STATIC UINTPTR GSataPhysBase = 0;
STATIC UINTPTR GSataVirtBase = 0;
STATIC PciDevice* GSataPciDev = NULLPTR;
STATIC SataPort* GSataPorts[32];
STATIC UINT32 GSataIrqGsi = 0;
STATIC BOOL GSataIrqRegistered = FALSE;

STATIC volatile UINT32 GSataIrqCount = 0;
STATIC volatile UINT32 GSataErrorCount = 0;

// ==================== DEBUG MACROS ====================
#define SATA_DEBUG_PRINT(fmt, ...) \
    BaseconPrintf(BASECON_TYPE_NORMAL, "sata: " fmt, ##__VA_ARGS__)
#define SATA_DEBUG_ERROR(fmt, ...) \
    BaseconPrintf(BASECON_TYPE_ERROR, "sata: ERROR: " fmt, ##__VA_ARGS__)

// ==================== FORWARD DECLARATIONS ====================
VOID SataRegisterDisk(SataPort *Port, INT PortNum);
STATIC INT SataAtapiInit(SataPort *Port);
STATIC INT SataAtapiPacketEx(SataPort *Port, const UINT8 *Cdb,
                             UINT32 ReadBytes, VOID *ReadBuf,
                             UINT32 WriteBytes, const VOID *WriteBuf);
STATIC INT SataDiskAtapiRead(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer);

// ==================== HELPER FUNCTIONS ====================
STATIC INLINE VOID SataMdelay(UINT32 Ms) {
    TimerMdelay(Ms);
}

STATIC INLINE VOID SataUdelay(UINT32 Us) {
    TimerUdelay(Us);
}

// Getting milliseconds based on HPET
STATIC INLINE UINT64 SataGetMs(VOID) {
    UINT32 TicksPerMs = TimerTicksPerMs();
    if (TicksPerMs == 0) {
        TicksPerMs = 1;  // Divide by zero protection
    }
    return TimerTicks() / TicksPerMs;
}

STATIC INLINE BOOL SataTimedOut(UINT64 StartMs, UINT32 TimeoutMs) {
    return (SataGetMs() - StartMs) >= TimeoutMs;
}

// Flush posted writes (as in Haiku)
STATIC INLINE VOID SataFlushPostedWrites(HbaPort* Port) {
    volatile UINT32 dummy = Port->Cmd;
    (VOID)dummy;
}

STATIC INLINE VOID SataFlushPostedWritesHba(HbaMem* Hba) {
    volatile UINT32 dummy = Hba->Ghc;
    (VOID)dummy;
}

// Memory barrier to guarantee the visibility of recordings to the device
STATIC INLINE VOID SataMemoryBarrier(VOID) {
    __sync_synchronize();
}

// Stop port (Haiku: Disable)
STATIC BOOL SataDisablePort(HbaPort* Port) {
    if ((Port->Cmd & SATA_HBA_PxCMD_ST) == 0) {
        return TRUE;
    }

    Port->Cmd &= ~SATA_HBA_PxCMD_ST;
    SataFlushPostedWrites(Port);

    UINT64 Start = SataGetMs();
    while ((Port->Cmd & SATA_HBA_PxCMD_CR) && !SataTimedOut(Start, 500)) {
        SataUdelay(1);
    }

    if (Port->Cmd & SATA_HBA_PxCMD_CR) {
        return FALSE;
    }

    return TRUE;
}

// Launching the port (Haiku: Enable)
STATIC BOOL SataEnablePort(HbaPort* Port) {
    if ((Port->Cmd & SATA_HBA_PxCMD_ST) != 0) {
        return TRUE;
    }

    if ((Port->Cmd & SATA_HBA_PxCMD_FRE) == 0) {
        return FALSE;
    }

    UINT64 Start = SataGetMs();
    while ((Port->Cmd & SATA_HBA_PxCMD_CR) && !SataTimedOut(Start, 500)) {
        SataUdelay(1);
    }

    if (Port->Cmd & SATA_HBA_PxCMD_CR) {
        return FALSE;
    }

    Port->Cmd |= SATA_HBA_PxCMD_ST;
    SataFlushPostedWrites(Port);
    return TRUE;
}

STATIC VOID SataStopCmd(HbaPort* Port) {
    SataDisablePort(Port);
}

STATIC VOID SataStartCmd(HbaPort* Port) {
    SataEnablePort(Port);
}

STATIC INT SataFindCmdSlot(HbaPort* Port) {
    UINT32 Slots = (Port->Sact | Port->Ci) & 0xFF;

    for (INT I = 0; I < 8; I++) {
        if (!(Slots & (1 << I))) {
            return I;
        }
    }

    return -1;
}

STATIC INT SataAcquireBuffer(SataPort* Port) {
    while (__sync_lock_test_and_set(&Port->BufferSemaphore, 1)) {
        CpuPause();
    }

    for (INT I = 0; I < 8; I++) {
        if (!__sync_lock_test_and_set(&Port->BufferLocks[I], 1)) {
            __sync_lock_release(&Port->BufferSemaphore);
            return I;
        }
    }

    __sync_lock_release(&Port->BufferSemaphore);
    return -1;
}

STATIC VOID SataReleaseBuffer(SataPort* Port, INT Index) {
    if (Index < 0 || Index >= 8) return;
    __sync_synchronize();
    __sync_lock_release(&Port->BufferLocks[Index]);
    __sync_lock_release(&Port->BufferSemaphore);
}

STATIC VOID SataPortLock(SataPort* Port) {
    while (__sync_lock_test_and_set(&Port->PortLock, 1)) {
        CpuPause();
    }
}

STATIC VOID SataPortUnlock(SataPort* Port) {
    __sync_synchronize();
    __sync_lock_release(&Port->PortLock);
}

STATIC BOOL SataAllocDmaPage(UINT64 *PhysOut, VOID **VirtOut) {
    VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
    if (!Phys) {
        return FALSE;
    }

    VOID *Virt = (VOID *)PhysToVirt((UINT64)Phys);
    MemSet(Virt, 0, PAGE_SIZE);
    if (PhysOut) {
        *PhysOut = (UINT64)Phys;
    }
    if (VirtOut) {
        *VirtOut = Virt;
    }
    return TRUE;
}

STATIC BOOL SataWaitTfdIdle(HbaPort *Regs, UINT32 TimeoutMs) {
    UINT64 Start = SataGetMs();
    while ((Regs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ)) &&
           !SataTimedOut(Start, TimeoutMs)) {
        SataUdelay(50);
    }
    return (Regs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ)) == 0;
}

STATIC BOOL SataClearBusyOverride(HbaPort *Regs) {
    if ((Regs->Tfd & (SATA_ATA_DEV_BUSY | SATA_ATA_DEV_DRQ)) == 0) {
        return TRUE;
    }

    Regs->Cmd |= SATA_HBA_PxCMD_CLO;
    SataFlushPostedWrites(Regs);

    UINT64 Start = SataGetMs();
    while ((Regs->Cmd & SATA_HBA_PxCMD_CLO) && !SataTimedOut(Start, 500)) {
        SataUdelay(10);
    }

    return (Regs->Cmd & SATA_HBA_PxCMD_CLO) == 0;
}

STATIC VOID SataRecoverPort(HbaPort *Regs) {
    SataDisablePort(Regs);
    Regs->Is = 0xFFFFFFFF;
    Regs->Serr = 0xFFFFFFFF;
    SataClearBusyOverride(Regs);
    SataEnablePort(Regs);
}

// ==================== IRQ HANDLER ====================
EXTERN(VOID, SataIrqStub(VOID));

// Handling port interrupts (Haiku: Interrupt)
STATIC VOID SataProcessPortInterrupt(INT PortNum) {
    SataPort* Port = GSataPorts[PortNum];
    if (!Port || Port->Status != SATA_PORT_ACTIVE) {
        return;
    }

    HbaPort* Regs = Port->Regs;
    UINT32 PortIs = Regs->Is;

    // Clear interrupts (write 1 to set bits)
    Regs->Is = PortIs;

    // Error Handling
    if (PortIs & SATA_HBA_PxIS_TFES) {
        GSataErrorCount++;
        Port->ErrorCount++;

        // Clearing errors
        Regs->Serr = 0xFFFFFFFF;

        // Unblock a waiting team
        if (Port->SlotWait[0].Active) {
            Port->SlotWait[0].Done = TRUE;
            Port->SlotWait[0].Result = -1;
        }

        return;
    }

    // Checking the completion of the command (Haiku: ci & 1)
    UINT32 Ci = Regs->Ci;
    if (Port->SlotWait[0].Active && !(Ci & 1)) {
        Port->SlotWait[0].Done = TRUE;
        Port->SlotWait[0].Result = 0;
    }
}

VOID SataIrqHandler(VOID) {
    GSataIrqCount++;

    if (!GSataHba) {
        ApicEoi();
        return;
    }

    UINT32 Is = GSataHba->Is & GSataHba->Pi;

    if (Is == 0) {
        ApicEoi();
        return;
    }

    // Clearing global interrupts
    GSataHba->Is = Is;

    // Processing ports
    for (INT PortNum = 0; PortNum < 32; PortNum++) {
        if (Is & (1 << PortNum)) {
            SataProcessPortInterrupt(PortNum);
        }
    }

    ApicEoi();
}

// ==================== COMMAND EXECUTION (ATA only) ====================
STATIC INT SataExecuteCommand(SataPort* Port, UINT32 Slot,
                               UINTPTR PhysBuffer, UINT32 ByteCount,
                               FisRegH2D* Fis, INT UseIrq) {
    HbaPort* Regs = Port->Regs;
    (VOID)UseIrq;

    if (!SataWaitTfdIdle(Regs, 5000)) {
        SATA_DEBUG_ERROR("device busy (TFD=0x%02X)\n", Regs->Tfd);
        if (!SataClearBusyOverride(Regs) || !SataWaitTfdIdle(Regs, 1000)) {
            return -1;
        }
    }

    if (Regs->Tfd & SATA_ATA_DEV_ERR) {
        Regs->Serr = 0xFFFFFFFF;
        Regs->Is = 0xFFFFFFFF;
    }

    HbaCmdHeader* CmdHeader = &Port->CmdList[Slot];
    UINT32 Ctba = CmdHeader->Ctba;
    UINT32 Ctbau = CmdHeader->Ctbau;
    MemSet((VOID *)CmdHeader, 0, sizeof(*CmdHeader));
    CmdHeader->Ctba = Ctba;
    CmdHeader->Ctbau = Ctbau;
    CmdHeader->Cfl = 5;
    CmdHeader->W = (Fis->Command == SATA_ATA_CMD_WRITE_DMA_EX) ? 1 : 0;
    CmdHeader->Prdtl = 1;
    CmdHeader->Prdbc = 0;

    HbaCmdTbl* CmdTbl = Port->CmdTables[Slot];
    MemSet(CmdTbl, 0, sizeof(*CmdTbl));
    MemCpy(CmdTbl->CFis, Fis, 20);

    CmdTbl->PrdtEntry[0].Dba = (UINT32)(PhysBuffer & 0xFFFFFFFF);
    CmdTbl->PrdtEntry[0].DbaU = (UINT32)((PhysBuffer >> 32) & 0xFFFFFFFF);
    CmdTbl->PrdtEntry[0].Dbc = ByteCount - 1;
    CmdTbl->PrdtEntry[0].I = 1;

    Regs->Is = 0xFFFFFFFF;
    Regs->Serr = 0xFFFFFFFF;
    SataMemoryBarrier();

    if (Regs->Ci & (1 << Slot)) {
        SATA_DEBUG_ERROR("slot %d still busy before command!\n", Slot);
        return -1;
    }

    SataMemoryBarrier();
    Regs->Ci = (1 << Slot);
    SataFlushPostedWrites(Regs);

    UINT64 Start = SataGetMs();
    while ((Regs->Ci & (1 << Slot)) && !SataTimedOut(Start, SATA_IRQ_TIMEOUT_MS)) {
        UINT32 Is = Regs->Is;
        if ((Is & SATA_HBA_PxIS_TFES) || (Regs->Tfd & SATA_ATA_DEV_ERR)) {
            SATA_DEBUG_ERROR("TF error: TFD=0x%02X, SER=0x%08X, IS=0x%08X\n",
                           Regs->Tfd, Regs->Serr, Is);
            SataRecoverPort(Regs);
            return -1;
        }
        SataUdelay(10);
    }

    if (Regs->Ci & (1 << Slot)) {
        SATA_DEBUG_ERROR("command timeout: Ci=0x%08X, TFD=0x%02X, CMD=0x%08X, IS=0x%08X\n",
                       Regs->Ci, Regs->Tfd, Regs->Cmd, Regs->Is);
        SataRecoverPort(Regs);
        return -1;
    }

    if (!SataWaitTfdIdle(Regs, 1000)) {
        SATA_DEBUG_ERROR("device busy after command (TFD=0x%02X)\n", Regs->Tfd);
        SataRecoverPort(Regs);
        return -1;
    }

    if (Regs->Tfd & SATA_ATA_DEV_ERR) {
        SATA_DEBUG_ERROR("command failed: TFD=0x%02X, SER=0x%08X\n",
                        Regs->Tfd, Regs->Serr);
        return -1;
    }

    return 0;
}

// ==================== IDENTIFY (ATA) ====================
STATIC VOID SataPortIdentify(SataPort* Port) {
    SATA_DEBUG_PRINT("port %d: identifying device...\n", Port->PortNum);

    HbaPort* Regs = Port->Regs;

    // Clearing the buffer
    MemSet(Port->VirtBuffers[0], 0, 512);

    // Create FIS for IDENTIFY (Haiku: SetATACommand)
    FisRegH2D Fis;
    MemSet(&Fis, 0, sizeof(Fis));
    Fis.FisType = SATA_FIS_TYPE_REG_H2D;
    Fis.C = 1;
    Fis.Command = SATA_ATA_CMD_IDENTIFY;
    Fis.Device = 0x40;

    INT Result = -1;
    for (INT Attempt = 0; Attempt < 3; Attempt++) {
        MemSet(Port->VirtBuffers[0], 0, 512);
        Result = SataExecuteCommand(Port, 0, Port->PhysBuffers[0], 512, &Fis, 0);
        if (Result == 0) {
            break;
        }
        SATA_DEBUG_ERROR("IDENTIFY attempt %d failed\n", Attempt + 1);
        SataMdelay(10);
    }

    if (Result != 0) {
        SATA_DEBUG_ERROR("IDENTIFY failed\n");
        return;
    }

    SATA_DEBUG_PRINT("IDENTIFY completed (TFD=0x%02X)\n", Regs->Tfd);

    // Parsing data
    UINT16* Identify = (UINT16*)Port->VirtBuffers[0];

    if (Identify[0] == 0x0000 || Identify[0] == 0xFFFF) {
        SATA_DEBUG_ERROR("invalid IDENTIFY signature: 0x%04X\n", Identify[0]);
        return;
    }

    // Model (Haiku: swap_words)
    CHAR Model[41];
    for (INT I = 0; I < 20; I++) {
        UINT16 Word = Identify[27 + I];
        Word = (Word << 8) | (Word >> 8);  // Swap bytes
        Model[I*2] = Word & 0xFF;
        Model[I*2 + 1] = (Word >> 8) & 0xFF;
    }
    Model[40] = '\0';

    INT Len = StrLen(Model);
    while (Len > 0 && Model[Len-1] == ' ') {
        Model[--Len] = '\0';
    }

    // Sector size
    Port->SectorSize = 512;
    if (Identify[106] & (1 << 12)) {
        if (Identify[106] & (1 << 14)) {
            Port->SectorSize = 4096;
        }
    }

    // Number of sectors
    if (Identify[83] & (1 << 10)) {
        Port->SupportsLba48 = 1;
        Port->TotalSectors = ((UINT64)Identify[103] << 48) |
                             ((UINT64)Identify[102] << 32) |
                             ((UINT64)Identify[101] << 16) |
                             (UINT64)Identify[100];
    } else {
        Port->SupportsLba48 = 0;
        Port->TotalSectors = ((UINT32)Identify[61] << 16) |
                             (UINT32)Identify[60];
    }

    if (Port->TotalSectors == 0) {
        SATA_DEBUG_ERROR("zero sectors\n");
        return;
    }

    SATA_DEBUG_PRINT("port %d: %s, %lld sectors, %d bytes/sector\n",
            Port->PortNum, Model, Port->TotalSectors, Port->SectorSize);
}

// ==================== PORT ACCESS (ATA only) ====================
STATIC INT SataPortAccess(SataPort* Port, UINT64 Lba, UINT32 Count,
                           UINTPTR PhysBuffer, INT Write) {
    SataPortLock(Port);

    UINTPTR Flags = SaveFlags();
    LocalInterruptsDisable();

    HbaPort* Regs = Port->Regs;

    INT Slot = SataFindCmdSlot(Regs);
    if (Slot == -1) {
        RestoreFlags(Flags);
        SataPortUnlock(Port);
        RETURN(INCORRECT_VALUE);
    }

    // Create FIS (Haiku: SetATA48Command)
    FisRegH2D Fis;
    MemSet(&Fis, 0, sizeof(Fis));
    Fis.FisType = SATA_FIS_TYPE_REG_H2D;
    Fis.C = 1;
    Fis.Command = Write ? SATA_ATA_CMD_WRITE_DMA_EX : SATA_ATA_CMD_READ_DMA_EX;

    // LBA48 (Haiku: SetATA48Command)
    Fis.Lba0 = (UINT8)(Lba & 0xFF);
    Fis.Lba1 = (UINT8)((Lba >> 8) & 0xFF);
    Fis.Lba2 = (UINT8)((Lba >> 16) & 0xFF);
    Fis.Device = 0x40;
    Fis.Lba3 = (UINT8)((Lba >> 24) & 0xFF);
    Fis.Lba4 = (UINT8)((Lba >> 32) & 0xFF);
    Fis.Lba5 = (UINT8)((Lba >> 40) & 0xFF);
    Fis.CountL = (UINT8)(Count & 0xFF);
    Fis.CountH = (UINT8)((Count >> 8) & 0xFF);

    UINT32 ByteCount = Port->SectorSize * Count;

    INT Result = SataExecuteCommand(Port, Slot, PhysBuffer, ByteCount, &Fis, 0);

    RestoreFlags(Flags);

    SataPortUnlock(Port);

    return (Result == 0) ? SUCCESS : DEVICE_ERROR;
}

// ---------- IDENTIFY PACKET DEVICE (0xA1) ----------
STATIC INT SataAtapiIdentify(SataPort *Port) {
    HbaPort *Regs = Port->Regs;

    INT BufIdx = SataAcquireBuffer(Port);
    if (BufIdx < 0) {
        SATA_DEBUG_ERROR("atapi: no free buffer for IDENTIFY PACKET\n");
        return NO_MEMORY;
    }

    MemSet(Port->VirtBuffers[BufIdx], 0, 512);

    FisRegH2D Fis;
    MemSet(&Fis, 0, sizeof(Fis));
    Fis.FisType = SATA_FIS_TYPE_REG_H2D;
    Fis.C = 1;
    Fis.Command = SATA_ATA_CMD_IDENTIFY_PACKET;
    Fis.Device = 0x40;

    INT Rc = -1;
    for (INT Attempt = 0; Attempt < 3; Attempt++) {
        MemSet(Port->VirtBuffers[BufIdx], 0, 512);
        Rc = SataExecuteCommand(Port, 0, Port->PhysBuffers[BufIdx],
                                512, &Fis, 0);
        if (Rc == 0) break;
        SATA_DEBUG_ERROR("atapi: IDENTIFY_PACKET attempt %d failed\n",
                         Attempt + 1);
        SataMdelay(10);
    }

    if (Rc != 0) {
        SataReleaseBuffer(Port, BufIdx);
        SATA_DEBUG_ERROR("atapi: IDENTIFY_PACKET failed (TFD=0x%02X)\n",
                         Regs->Tfd);
        return DEVICE_ERROR;
    }

    UINT16 *Ident = (UINT16 *)Port->VirtBuffers[BufIdx];

    /* Model (words 27..46), byte-swapped like ATA */
    CHAR Model[41];
    for (INT I = 0; I < 20; I++) {
        UINT16 W = Ident[27 + I];
        Model[I*2]     = (CHAR)(W >> 8);
        Model[I*2 + 1] = (CHAR)(W & 0xFF);
    }
    Model[40] = '\0';
    INT Len = StrLen(Model);
    while (Len > 0 && Model[Len-1] == ' ') Model[--Len] = '\0';

    SATA_DEBUG_PRINT("atapi: port %d: model = '%s'\n", Port->PortNum, Model);

    /* ATAPI has no concept of "TotalSectors" before READ CAPACITY,
       That's why we don't fill in anything here. */
    Port->SectorSize      = 2048;
    Port->AtapiSectorSize = 2048;
    Port->SupportsLba48   = 0;

    SataReleaseBuffer(Port, BufIdx);
    return SUCCESS;
}

// ---------- PACKET (0xA0) ----------
STATIC INT SataAtapiPacketEx(SataPort *Port, const UINT8 *Cdb,
                             UINT32 ReadBytes, VOID *ReadBuf,
                             UINT32 WriteBytes, const VOID *WriteBuf) {
    if (!Port || !Cdb) return DEVICE_INVALID;
    if (Port->Status != SATA_PORT_ACTIVE) return NO_OBJECT;
    if (ReadBytes && WriteBytes) return INCORRECT_VALUE;
    if (ReadBytes && !ReadBuf) return INCORRECT_VALUE;
    if (WriteBytes && !WriteBuf) return INCORRECT_VALUE;

    HbaPort *Regs = Port->Regs;

    if (!SataWaitTfdIdle(Regs, 5000)) {
        SATA_DEBUG_ERROR("atapi: device busy (TFD=0x%02X)\n", Regs->Tfd);
        if (!SataClearBusyOverride(Regs) || !SataWaitTfdIdle(Regs, 1000)) {
            return DEVICE_ERROR;
        }
    }

    INT Slot = SataFindCmdSlot(Regs);
    if (Slot == -1) return INCORRECT_VALUE;

    HbaCmdHeader *Hdr = &Port->CmdList[Slot];
    UINT32 Ctba  = Hdr->Ctba;
    UINT32 Ctbau = Hdr->Ctbau;
    MemSet((VOID *)Hdr, 0, sizeof(*Hdr));
    Hdr->Ctba  = Ctba;
    Hdr->Ctbau = Ctbau;
    Hdr->Cfl   = 5;
    Hdr->A     = 1;
    Hdr->W     = (WriteBytes > 0) ? 1 : 0;
    Hdr->Prdtl = (ReadBytes || WriteBytes) ? 1 : 0;
    Hdr->Prdbc = 0;

    HbaCmdTbl *Tbl = Port->CmdTables[Slot];
    MemSet(Tbl, 0, sizeof(*Tbl));

    UINT32 ByteCount = ReadBytes ? ReadBytes : WriteBytes;
    if (ByteCount > 0xFFFF) ByteCount = 0xFFFF;

    // Защита от переполнения 4К буфера
    if (ByteCount > PAGE_SIZE) {
        SATA_DEBUG_ERROR("atapi: ByteCount %u exceeds page size\n", ByteCount);
        return INCORRECT_VALUE;
    }

    FisRegH2D Fis;
    MemSet(&Fis, 0, sizeof(Fis));
    Fis.FisType  = SATA_FIS_TYPE_REG_H2D;
    Fis.C        = 1;
    Fis.Command  = SATA_ATA_CMD_PACKET;
    Fis.Device   = 0x40;
    Fis.FeatureL = (UINT8)(ByteCount & 0xFF);
    Fis.FeatureH = (UINT8)((ByteCount >> 8) & 0xFF);
    MemCpy(Tbl->CFis, &Fis, 20);

    MemCpy(Tbl->ACmd, Cdb, 12);

    INT BufIdx = -1;
    UINTPTR PhysBuf = 0;
    VOID   *VirtBuf = NULLPTR;
    UINT32  BytesToMove = 0;

    if (ReadBytes || WriteBytes) {
        BufIdx = SataAcquireBuffer(Port);
        if (BufIdx < 0) return INCORRECT_VALUE;

        BytesToMove = ReadBytes ? ReadBytes : WriteBytes;
        PhysBuf = Port->PhysBuffers[BufIdx];
        VirtBuf = Port->VirtBuffers[BufIdx];

        if (WriteBytes) {
            MemCpy(VirtBuf, WriteBuf, BytesToMove);
        }

        Tbl->PrdtEntry[0].Dba  = (UINT32)(PhysBuf & 0xFFFFFFFF);
        Tbl->PrdtEntry[0].DbaU = (UINT32)((PhysBuf >> 32) & 0xFFFFFFFF);
        Tbl->PrdtEntry[0].Dbc  = BytesToMove - 1;
        Tbl->PrdtEntry[0].I    = 1;
    }

    Regs->Is   = 0xFFFFFFFF;
    Regs->Serr = 0xFFFFFFFF;
    SataMemoryBarrier();

    if (Regs->Ci & (1 << Slot)) {
        SATA_DEBUG_ERROR("atapi: slot %d still busy\n", Slot);
        if (BufIdx >= 0) SataReleaseBuffer(Port, BufIdx);
        return INCORRECT_VALUE;
    }

    SataMemoryBarrier();
    Regs->Ci = (1 << Slot);
    SataMemoryBarrier();
    SataFlushPostedWrites(Regs);

    UINT64 Start = SataGetMs();
    BOOL IsError = FALSE;

    while (!SataTimedOut(Start, SATA_IRQ_TIMEOUT_MS)) {
        UINT32 Ci = Regs->Ci;
        if (!(Ci & (1 << Slot))) {
            break;
        }

        UINT32 Is = Regs->Is;
        if ((Is & SATA_HBA_PxIS_TFES) ||
            (Regs->Tfd & (SATA_ATA_DEV_ERR | SATA_ATA_DEV_BUSY))) {
            SataUdelay(50);
            if (Regs->Ci & (1 << Slot)) {
                IsError = TRUE;
                break;
            }
            break;
        }

        SataUdelay(50);
    }

    if (Regs->Ci & (1 << Slot)) {
        SATA_DEBUG_ERROR("atapi: timeout: Ci=0x%08X TFD=0x%08X IS=0x%08X\n",
                         Regs->Ci, Regs->Tfd, Regs->Is);
        SataRecoverPort(Regs);
        if (BufIdx >= 0) SataReleaseBuffer(Port, BufIdx);
        return TIMEOUT;
    }

    UINT32 Is = Regs->Is;
    if (IsError || (Is & SATA_HBA_PxIS_TFES) || (Regs->Tfd & SATA_ATA_DEV_ERR)) {
        SATA_DEBUG_ERROR("atapi: device error: TFD=0x%02X IS=0x%08X SER=0x%08X\n",
                         Regs->Tfd, Is, Regs->Serr);
        SataRecoverPort(Regs);
        if (BufIdx >= 0) SataReleaseBuffer(Port, BufIdx);
        return DEVICE_ERROR;
    }

    if (ReadBytes && VirtBuf) {
        MemCpy(ReadBuf, VirtBuf, BytesToMove);
    }

    if (BufIdx >= 0) {
        SataReleaseBuffer(Port, BufIdx);
    }

    return SUCCESS;
}

// ---------- SCSI commands ----------

INT SataAtapiRequestSense(SataPort *Port) {
    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_REQUEST_SENSE;
    Cdb[4] = 18;

    UINT8 Sense[18];
    MemSet(Sense, 0, 18);

    INT Rc = SataAtapiPacketEx(Port, Cdb, 18, Sense, 0, NULLPTR);
    if (Rc != SUCCESS) return Rc;

    Port->AtapiSenseKey = Sense[2] & 0x0F;
    Port->AtapiAsc      = Sense[12];
    Port->AtapiAscq     = Sense[13];

    if (Port->AtapiSenseKey == 0x02 && Port->AtapiAsc == 0x3A) {
        Port->AtapiMediaPresent = FALSE;
    } else if (Port->AtapiSenseKey == 0x06) {
        Port->AtapiMediaPresent = FALSE;
    }

    return SUCCESS;
}

INT SataAtapiTestUnitReady(SataPort *Port) {
    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_TEST_UNIT_READY;

    INT Rc = SataAtapiPacketEx(Port, Cdb, 0, NULLPTR, 0, NULLPTR);
    if (Rc == SUCCESS) {
        Port->AtapiMediaPresent = TRUE;
        return SUCCESS;
    }

    SataAtapiRequestSense(Port);

    if (Port->AtapiSenseKey == 0x02 && Port->AtapiAsc == 0x3A) {
        Port->AtapiMediaPresent = FALSE;
        return NO_OBJECT;
    }

    return Rc;
}

INT SataAtapiInquiry(SataPort *Port) {
    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_INQUIRY;
    Cdb[4] = 36;

    UINT8 Resp[36];
    MemSet(Resp, 0, 36);

    INT Rc = SataAtapiPacketEx(Port, Cdb, 36, Resp, 0, NULLPTR);
    if (Rc != SUCCESS) return Rc;

    MemCpy(Port->AtapiVendor,   Resp + 8,  8);
    Port->AtapiVendor[8] = '\0';
    MemCpy(Port->AtapiProduct,  Resp + 16, 16);
    Port->AtapiProduct[16] = '\0';
    MemCpy(Port->AtapiRevision, Resp + 32, 4);
    Port->AtapiRevision[4] = '\0';

    for (INT I = 7;  I >= 0  && Port->AtapiVendor[I]   == ' '; I--) Port->AtapiVendor[I]   = '\0';
    for (INT I = 15; I >= 0  && Port->AtapiProduct[I]  == ' '; I--) Port->AtapiProduct[I]  = '\0';
    for (INT I = 3;  I >= 0  && Port->AtapiRevision[I] == ' '; I--) Port->AtapiRevision[I] = '\0';

    SATA_DEBUG_PRINT("atapi: %s %s %s\n",
                     Port->AtapiVendor, Port->AtapiProduct, Port->AtapiRevision);
    return SUCCESS;
}

INT SataAtapiReadCapacity(SataPort *Port) {
    UINT8 Cdb[12];
    MemSet(Cdb, 0, 12);
    Cdb[0] = SCSI_READ_CAPACITY_10;

    UINT8 Resp[8];
    MemSet(Resp, 0, 8);

    INT Rc = SataAtapiPacketEx(Port, Cdb, 8, Resp, 0, NULLPTR);
    if (Rc != SUCCESS) return Rc;

    UINT32 LastLba = ((UINT32)Resp[0] << 24) | ((UINT32)Resp[1] << 16) |
                     ((UINT32)Resp[2] << 8)  |  (UINT32)Resp[3];
    UINT32 BlockSize = ((UINT32)Resp[4] << 24) | ((UINT32)Resp[5] << 16) |
                       ((UINT32)Resp[6] << 8)  |  (UINT32)Resp[7];

    if (BlockSize == 0 || BlockSize > 8192) BlockSize = 2048;

    Port->AtapiSectorSize = BlockSize;
    Port->SectorSize      = (UINT16)BlockSize;
    Port->TotalSectors    = (UINT64)LastLba + 1;

    SATA_DEBUG_PRINT("atapi: capacity: %llu sectors x %u bytes (%llu MB)\n",
                     Port->TotalSectors, BlockSize,
                     (Port->TotalSectors * BlockSize) / (1024 * 1024));
    return SUCCESS;
}

INT SataAtapiReadSectors(SataPort *Port, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Port || !Buffer || Count == 0) return DEVICE_INVALID;
    if (Port->Status != SATA_PORT_ACTIVE) return NO_OBJECT;
    if (!Port->AtapiMediaPresent)         return NO_OBJECT;
    if (Lba + Count > Port->TotalSectors) return INCORRECT_VALUE;

    UINT32 SectorSize = Port->AtapiSectorSize ? Port->AtapiSectorSize : 2048;
    UINT8 *Out = (UINT8 *)Buffer;
    UINT64 CurLba = Lba;
    UINT32 Remaining = Count;

    while (Remaining > 0) {
        // Читаем строго по 1 сектору. 1 сектор (2048) влезает в 4К буфер.
        // Если SectorSize = 4096, то тоже 1 сектор (4096), что впритык.
        UINT16 ThisCount = 1;

        UINT8 Cdb[12];
        MemSet(Cdb, 0, 12);
        Cdb[0] = SCSI_READ_10;
        Cdb[2] = (UINT8)((CurLba >> 24) & 0xFF);
        Cdb[3] = (UINT8)((CurLba >> 16) & 0xFF);
        Cdb[4] = (UINT8)((CurLba >>  8) & 0xFF);
        Cdb[5] = (UINT8)((CurLba      ) & 0xFF);
        Cdb[7] = (UINT8)((ThisCount >> 8) & 0xFF);
        Cdb[8] = (UINT8)((ThisCount     ) & 0xFF);

        UINT32 BytesThis = (UINT32)ThisCount * SectorSize;

        INT Rc = SataAtapiPacketEx(Port, Cdb, BytesThis, Out, 0, NULLPTR);
        if (Rc != SUCCESS) {
            SataAtapiRequestSense(Port);
            // Одна повторная попытка
            Rc = SataAtapiPacketEx(Port, Cdb, BytesThis, Out, 0, NULLPTR);
            if (Rc != SUCCESS) return Rc;
        }

        Out       += BytesThis;
        CurLba    += ThisCount;
        Remaining -= ThisCount;
    }

    return SUCCESS;
}

// ---------- Initializing the ATAPI port ----------
STATIC INT SataAtapiInit(SataPort *Port) {
    SATA_DEBUG_PRINT("atapi: port %d: initializing...\n", Port->PortNum);

    Port->IsAtapi          = TRUE;
    Port->AtapiSectorSize  = 2048;
    Port->SectorSize       = 2048;
    Port->AtapiMediaPresent = FALSE;
    Port->AtapiSenseKey    = 0;
    Port->AtapiAsc         = 0;
    Port->AtapiAscq        = 0;
    Port->TotalSectors     = 0;

    SataAtapiIdentify(Port);
    SataAtapiInquiry(Port);

    INT Rc = SataAtapiTestUnitReady(Port);
    if (Rc == SUCCESS) {
        SataAtapiReadCapacity(Port);
    } else {
        SATA_DEBUG_PRINT("atapi: port %d: no media inserted\n", Port->PortNum);
    }

    return SUCCESS;
}

INT SataAtapiRescan(SataPort *Port) {
    if (!Port || !Port->IsAtapi) return DEVICE_INVALID;
    INT Rc = SataAtapiTestUnitReady(Port);
    if (Rc == SUCCESS) {
        SataAtapiReadCapacity(Port);
    }
    return Rc;
}

// ==================== PORT INITIALIZATION ====================
STATIC SataPort* SataPortInit(INT PortNum, HbaPort* PortRegs, HbaMem* Hba) {
    SATA_DEBUG_PRINT("initializing port %d...\n", PortNum);

    SataPort* Port = (SataPort*)MemoryAllocate(sizeof(SataPort));
    if (!Port) {
        return NULLPTR;
    }

    MemSet(Port, 0, sizeof(SataPort));
    Port->Regs = PortRegs;
    Port->PortNum = PortNum;
    Port->Status = SATA_PORT_UNINITIALIZED;
    Port->SectorSize = 512;

    if (PortRegs->Cmd & SATA_HBA_PxCMD_ST) {
        PortRegs->Cmd &= ~SATA_HBA_PxCMD_ST;
        SataFlushPostedWrites(PortRegs);
        UINT64 Timeout = SataGetMs() + 500;
        while ((PortRegs->Cmd & SATA_HBA_PxCMD_CR) && SataGetMs() < Timeout) {
            SataUdelay(1);
        }
    }

    if (PortRegs->Cmd & SATA_HBA_PxCMD_FRE) {
        PortRegs->Cmd &= ~SATA_HBA_PxCMD_FRE;
        SataFlushPostedWrites(PortRegs);
        UINT64 Timeout = SataGetMs() + 500;
        while ((PortRegs->Cmd & SATA_HBA_PxCMD_FR) && SataGetMs() < Timeout) {
            SataUdelay(1);
        }
    }

    UINT64 CmdListPhysAddr;
    VOID* CmdListVirtAddr;
    if (!SataAllocDmaPage(&CmdListPhysAddr, &CmdListVirtAddr)) {
        MemoryFree(Port);
        return NULLPTR;
    }
    PortRegs->Clb = (UINT32)(CmdListPhysAddr & 0xFFFFFFFF);
    PortRegs->Clbu = (UINT32)(CmdListPhysAddr >> 32);
    Port->CmdList = (HbaCmdHeader*)CmdListVirtAddr;

    UINT64 FisPhysAddr;
    VOID* FisVirtAddr;
    if (!SataAllocDmaPage(&FisPhysAddr, &FisVirtAddr)) {
        MemoryFree(Port);
        return NULLPTR;
    }
    PortRegs->Fb = (UINT32)(FisPhysAddr & 0xFFFFFFFF);
    PortRegs->Fbu = (UINT32)(FisPhysAddr >> 32);
    Port->Fis = (HbaFis*)FisVirtAddr;

    MemSet((VOID*)Port->Fis, 0, sizeof(HbaFis));
    Port->Fis->DsFis.FisType = SATA_FIS_TYPE_DMA_SETUP;
    Port->Fis->PsFis.FisType = SATA_FIS_TYPE_PIO_SETUP;
    Port->Fis->RFis.FisType = SATA_FIS_TYPE_REG_D2H;
    Port->Fis->SdbFis[0] = SATA_FIS_TYPE_DEV_BITS;

    for (INT I = 0; I < 8; I++) {
        UINT64 TblPhysAddr;
        VOID* TblVirtAddr;
        if (!SataAllocDmaPage(&TblPhysAddr, &TblVirtAddr)) {
            MemoryFree(Port);
            return NULLPTR;
        }
        Port->CmdList[I].Ctba = (UINT32)(TblPhysAddr & 0xFFFFFFFF);
        Port->CmdList[I].Ctbau = (UINT32)(TblPhysAddr >> 32);
        Port->CmdTables[I] = (HbaCmdTbl*)TblVirtAddr;

        UINT64 BufPhysAddr;
        VOID* BufVirtAddr;
        if (!SataAllocDmaPage(&BufPhysAddr, &BufVirtAddr)) {
            MemoryFree(Port);
            return NULLPTR;
        }
        Port->PhysBuffers[I] = (UINTPTR)BufPhysAddr;
        Port->VirtBuffers[I] = BufVirtAddr;
    }

    PortRegs->Sctl |= (SATA_SCTL_PORT_IPM_NOPART | SATA_SCTL_PORT_IPM_NOSLUM);
    SataFlushPostedWrites(PortRegs);

    PortRegs->Is = PortRegs->Is;
    PortRegs->Serr = 0xFFFFFFFF;

    PortRegs->Cmd |= SATA_HBA_PxCMD_FRE;
    SataFlushPostedWrites(PortRegs);

    UINT64 Timeout = SataGetMs() + 500;
    while (!(PortRegs->Cmd & SATA_HBA_PxCMD_FR) && SataGetMs() < Timeout) {
        SataUdelay(1);
    }

    if (!(PortRegs->Cmd & SATA_HBA_PxCMD_FR)) {
        SATA_DEBUG_ERROR("port %d: FIS receive enable failed\n", PortNum);
        Port->Status = SATA_PORT_ERROR;
        return Port;
    }

    PortRegs->Cmd |= SATA_HBA_PxCMD_ST;
    SataFlushPostedWrites(PortRegs);

    Timeout = SataGetMs() + 500;
    while (!(PortRegs->Cmd & SATA_HBA_PxCMD_CR) && SataGetMs() < Timeout) {
        SataUdelay(1);
    }

    if (!(PortRegs->Cmd & SATA_HBA_PxCMD_CR)) {
        SATA_DEBUG_ERROR("port %d: command engine start failed\n", PortNum);
        Port->Status = SATA_PORT_ERROR;
        return Port;
    }

    Timeout = SataGetMs() + 1000;
    while ((PortRegs->Tfd & SATA_ATA_DEV_BUSY) && SataGetMs() < Timeout) {
        SataMdelay(1);
    }

    if (PortRegs->Tfd & SATA_ATA_DEV_BUSY) {
        SATA_DEBUG_ERROR("port %d: device busy after spin-up (TFD=0x%02X)\n",
                        PortNum, PortRegs->Tfd);
        Port->Status = SATA_PORT_ERROR;
        return Port;
    }

    UINT32 Det = PortRegs->Ssts & SATA_HBA_PxSSTS_DET;
    if (Det != SATA_HBA_PxSSTS_DET_PRESENT) {
        SATA_DEBUG_ERROR("port %d: no device present (SSTS=0x%08X)\n",
                        PortNum, PortRegs->Ssts);
        Port->Status = SATA_PORT_ERROR;
        return Port;
    }

    PortRegs->Ie = SATA_PXIE_DHRE | SATA_PXIE_TFEE | SATA_PXIE_PCE;
    SataFlushPostedWrites(PortRegs);

    Port->Status = SATA_PORT_ACTIVE;

    Port->Sig = PortRegs->Sig;
    Port->IsAtapi = FALSE;

    if (Port->Sig == SATA_SIG_ATAPI) {
        SataAtapiInit(Port);
    } else {
        SataPortIdentify(Port);
    }

    return Port;
}

// ==================== INIT ====================
INT SataInit(VOID) {
    SATA_DEBUG_PRINT("initializing SATA/AHCI...\n");

    GSataPciDev = PciFindClass(0x01, 0x06);
    if (!GSataPciDev || GSataPciDev->ProgIf != 0x01) {
        SATA_DEBUG_ERROR("no AHCI controller\n");
        RETURN(NOT_FOUND);
    }

    SATA_DEBUG_PRINT("found AHCI: 0x%04X:0x%04X\n",
                    GSataPciDev->VendorId, GSataPciDev->DeviceId);

    PciEnable(GSataPciDev);
    PciEnableBusmaster(GSataPciDev);

    GSataPhysBase = GSataPciDev->Bars[5] & ~0xF;
    GSataVirtBase = PhysToVirt(GSataPhysBase);
    GSataHba = (HbaMem*)GSataVirtBase;

    GSataHba->Ghc |= SATA_GHC_ENABLE;
    SataFlushPostedWritesHba(GSataHba);

    UINT64 Timeout = SataGetMs() + 100;
    while (!(GSataHba->Ghc & SATA_GHC_ENABLE) && SataGetMs() < Timeout) {
        SataMdelay(1);
    }

    if (!(GSataHba->Ghc & SATA_GHC_ENABLE)) {
        SATA_DEBUG_ERROR("failed to enable HBA\n");
        RETURN(DEVICE_ERROR);
    }

    GSataHba->Ghc &= ~SATA_GHC_IE;
    SataFlushPostedWritesHba(GSataHba);

    GSataHba->Is = 0xFFFFFFFF;

    UINT32 Pi = GSataHba->Pi;
    INT ActiveCount = 0;

    for (INT I = 0; I < 32; I++) {
        if (!(Pi & (1 << I))) {
            continue;
        }

        UINT32 Ipm = (GSataHba->Ports[I].Ssts >> 8) & 0x0F;
        UINT32 Det = GSataHba->Ports[I].Ssts & SATA_HBA_PxSSTS_DET;

        if (Ipm != SATA_HBA_PORT_IPM_ACTIVE ||
            Det != SATA_HBA_PxSSTS_DET_PRESENT) {
            continue;
        }

        SataPort* Port = SataPortInit(I, &GSataHba->Ports[I], GSataHba);
        if (Port && Port->Status == SATA_PORT_ACTIVE) {
            GSataPorts[I] = Port;
            ActiveCount++;
            SataRegisterDisk(Port, I);
        }
    }

    if (GSataPciDev->InterruptLine > 0 && GSataPciDev->InterruptLine < 16) {
        UINT32 Flags = IOAPIC_FLAG_EDGE_TRIGGERED | IOAPIC_FLAG_ACTIVE_HIGH;

        if (IoapicGetOverride(GSataPciDev->InterruptLine, &GSataIrqGsi, &Flags) != SUCCESS) {
            GSataIrqGsi = GSataPciDev->InterruptLine;
        }

        IdtSetGate(SATA_IRQ_VECTOR, (VOID(*)())SataIrqStub,
                   KERNEL_CODE_SEL, IDT_GATE_INT, 0);

        if (IoapicRedirectIrq(GSataIrqGsi, SATA_IRQ_VECTOR,
                              ApicGetId(), Flags) == SUCCESS) {
            IoapicUnmaskIrq(GSataIrqGsi);
            GSataIrqRegistered = TRUE;
        }
    }

    GSataHba->Ghc |= SATA_GHC_IE;
    SataFlushPostedWritesHba(GSataHba);

    SATA_DEBUG_PRINT("initialized: %d active port(s)\n", ActiveCount);

    RETURN(SUCCESS);
}

// ==================== DISK OPERATIONS ====================
INT SataPortRead(SataPort *Port, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Port || Port->Status != SATA_PORT_ACTIVE) {
        RETURN(NO_OBJECT);
    }

    if (Port->IsAtapi) {
        return SataAtapiReadSectors(Port, Lba, Count, Buffer);
    }

    // ОРИГИНАЛЬНЫЙ КОД ДЛЯ ATA (без изменений)
    UINT32 SectorSize = Port->SectorSize;
    UINT8* BufPtr = (UINT8*)Buffer;
    UINT64 Remaining = (Count + 8 - 1) / 8; // Сколько раз читать по 8 секторов? 

    // Примечание: в оригинале было Remaining = Count; и цикл while (Remaining > 0) с SectorsThis = 8.
    // Я восстанавливаю ровно оригинальную логику, чтобы не сломать чтение.
    UINT64 RemainingSectors = Count;
    
    while (RemainingSectors > 0) {
        UINT32 SectorsThis = (RemainingSectors > 8) ? 8 : (UINT32)RemainingSectors;
        UINT32 BytesThis = SectorsThis * SectorSize;

        INT BufIdx = SataAcquireBuffer(Port);
        if (BufIdx < 0) {
            RETURN(INCORRECT_VALUE);
        }

        INT Ret = SataPortAccess(Port, Lba, SectorsThis,
                                  Port->PhysBuffers[BufIdx], 0);

        if (Ret == SUCCESS) {
            MemCpy(BufPtr, Port->VirtBuffers[BufIdx], BytesThis);
        }

        SataReleaseBuffer(Port, BufIdx);

        if (Ret != SUCCESS) {
            return Ret;
        }

        BufPtr += BytesThis;
        Lba += SectorsThis;
        RemainingSectors -= SectorsThis;
    }

    RETURN(SUCCESS);
}

INT SataPortWrite(SataPort *Port, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Port || Port->Status != SATA_PORT_ACTIVE) {
        RETURN(NO_OBJECT);
    }

    if (Port->IsAtapi) {
        RETURN(NOT_SUPPORTED);
    }

    // ОРИГИНАЛЬНЫЙ КОД ДЛЯ ATA (без изменений)
    UINT32 SectorSize = Port->SectorSize;
    const UINT8* BufPtr = (const UINT8*)Buffer;
    UINT64 RemainingSectors = Count;

    while (RemainingSectors > 0) {
        UINT32 SectorsThis = (RemainingSectors > 8) ? 8 : (UINT32)RemainingSectors;
        UINT32 BytesThis = SectorsThis * SectorSize;

        INT BufIdx = SataAcquireBuffer(Port);
        if (BufIdx < 0) {
            RETURN(INCORRECT_VALUE);
        }

        MemCpy(Port->VirtBuffers[BufIdx], BufPtr, BytesThis);

        INT Ret = SataPortAccess(Port, Lba, SectorsThis,
                                  Port->PhysBuffers[BufIdx], 1);

        SataReleaseBuffer(Port, BufIdx);

        if (Ret != SUCCESS) {
            return Ret;
        }

        BufPtr += BytesThis;
        Lba += SectorsThis;
        RemainingSectors -= SectorsThis;
    }

    RETURN(SUCCESS);
}

SataPort* SataGetPort(INT PortNum) {
    if (PortNum < 0 || PortNum >= 32) return NULLPTR;
    return GSataPorts[PortNum];
}

VOID SataGetStats(UINT32 *IrqCount, UINT32 *ErrorCount) {
    if (IrqCount) *IrqCount = GSataIrqCount;
    if (ErrorCount) *ErrorCount = GSataErrorCount;
}

STATIC INT SataDiskRead(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    SataPort *Port = (SataPort*)Disk->DriverData;
    return SataPortRead(Port, Lba, Count, Buffer);
}

STATIC INT SataDiskWrite(Disk *Disk, UINT64 Lba, UINT32 Count, const VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    SataPort *Port = (SataPort*)Disk->DriverData;
    return SataPortWrite(Port, Lba, Count, Buffer);
}

STATIC INT SataDiskFlush(Disk *Disk) {
    (VOID)Disk;
    return SUCCESS;
}

STATIC INT SataDiskAtapiRead(Disk *Disk, UINT64 Lba, UINT32 Count, VOID *Buffer) {
    if (!Disk || !Disk->DriverData) return DEVICE_INVALID;
    SataPort *Port = (SataPort *)Disk->DriverData;
    return SataAtapiReadSectors(Port, Lba, Count, Buffer);
}

VOID SataRegisterDisk(SataPort *Port, INT PortNum) {
    if (!Port) return;

    BOOL IsAtapi = Port->IsAtapi;

    if (!IsAtapi && Port->TotalSectors == 0) return;

    Disk *SataDisk = (Disk*)MemoryAllocate(sizeof(Disk));
    if (!SataDisk) return;

    MemSet(SataDisk, 0, sizeof(Disk));

    SataDisk->Type  = IsAtapi ? DISK_TYPE_OPTICAL : DISK_TYPE_SATA;
    SataDisk->Index = PortNum;

    SnPrintf(SataDisk->Name, sizeof(SataDisk->Name),
             IsAtapi ? "SATAPI%d" : "SATA%d", PortNum);

    SataDisk->SectorCount = Port->TotalSectors;
    SataDisk->SectorSize  = Port->SectorSize ? Port->SectorSize : 512;
    SataDisk->TotalSize   = Port->TotalSectors * SataDisk->SectorSize;
    SataDisk->DriverData  = Port;
    SataDisk->Initialized = TRUE;

    if (IsAtapi) {
        SataDisk->Read  = SataDiskAtapiRead;
        SataDisk->Write = NULLPTR;
        SataDisk->Flush = SataDiskFlush;
    } else {
        SataDisk->Read  = SataDiskRead;
        SataDisk->Write = SataDiskWrite;
        SataDisk->Flush = SataDiskFlush;
    }

    if (DiskMgrRegisterDisk(SataDisk) == SUCCESS) {
        BaseconPrintf(BASECON_TYPE_NORMAL, "sata: registered %s (%llu MB)\n",
                      SataDisk->Name, SataDisk->TotalSize / (1024 * 1024));
    } else {
        MemoryFree(SataDisk);
    }
}