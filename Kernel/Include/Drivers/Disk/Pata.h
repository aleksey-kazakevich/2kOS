#pragma once

#include <Types.h>
#include <Lib/String.h>
#include <Drivers/Pci.h>
#include <SpinLock.h>

/*
 * Basic ports (compatibility mode)
 */
#define PATA_BASE_PRIMARY 0x1F0
#define PATA_CTRL_PRIMARY 0x3F6
#define PATA_BASE_SECONDARY 0x170
#define PATA_CTRL_SECONDARY 0x376

/*
 * Register offsets relative to base/ctrl
 */
#define PATA_DATA 0x00
#define PATA_ERROR 0x01
#define PATA_FEATURE 0x01
#define PATA_NSECT 0x02
#define PATA_SECTOR 0x03
#define PATA_LCYL 0x04
#define PATA_HCYL 0x05
#define PATA_SELECT 0x06
#define PATA_STATUS 0x07
#define PATA_COMMAND 0x07

/*
 * ctrl port
 */
#define PATA_ALTSTATUS 0x0
#define PATA_CONTROL 0x0

/*
 * Status bits
 */
#define PATA_STATUS_BSY 0x80
#define PATA_STATUS_DRDY 0x40
#define PATA_STATUS_DRQ 0x08
#define PATA_STATUS_ERR 0x01
#define PATA_STATUS_DF 0x20

/*
 * Commands
 */
#define PATA_CMD_READ_SECTORS 0x20
#define PATA_CMD_WRITE_SECTORS 0x30
#define PATA_CMD_IDENTIFY 0xEC
#define PATA_CMD_IDENTIFY_PACKET 0xA1
#define PATA_CMD_READ_SECTORS_EXT 0x24
#define PATA_CMD_WRITE_SECTORS_EXT 0x34
#define PATA_CMD_CACHE_FLUSH 0xE7
#define PATA_CMD_CACHE_FLUSH_EXT 0xEA

/*
 * DMA Commands
 */
#define PATA_CMD_READ_DMA       0xC8
#define PATA_CMD_WRITE_DMA      0xCA
#define PATA_CMD_READ_DMA_EXT   0x25
#define PATA_CMD_WRITE_DMA_EXT  0x35
#define PATA_CMD_PACKET         0xA0

/*
 * Bus Master Registers (offset from BAR4)
 */
#define BMCR        0x00
#define BMSR        0x02
#define BMIDETBL    0x04

#define BMCR_START  (1 << 0)
#define BMCR_READ   (1 << 3)
#define BMCR_RESET  (1 << 2)

#define BMSR_DRQ    (1 << 2)
#define BMSR_ERROR  (1 << 1)
#define BMSR_INTR   (1 << 0)

/*
 * PRDT entry
 */
#define PRDT_ENTRY_COUNT 16
#define PRDT_EOT         0x8000
#define MAX_DMA_SECTORS   (PRDT_ENTRY_COUNT * 63)

/*
 * Timeout (poll iterations)
 */
#define PATA_TIMEOUT_LOOPS 100000U
#define PATA_DMA_TIMEOUT_MS 5000
#define PATA_PIO_TIMEOUT_MS 10000

typedef enum {
    PATA_CHANNEL_PRIMARY = 0,
    PATA_CHANNEL_SECONDARY = 1
} PataChannel;

typedef enum {
    PATA_TYPE_NONE = 0,
    PATA_TYPE_ATA,
    PATA_TYPE_ATAPI
} PataDeviceType;

typedef enum {
    PATA_OP_NONE = 0,
    PATA_OP_READ,
    PATA_OP_WRITE,
    PATA_OP_IDENTIFY
} PataOperation;

/*
 * PRDT entry structure
 */
typedef struct {
    UINT32 PhysAddr;
    UINT16 ByteCount;
    UINT16 EndOfTable;
} ATTRIBUTE(packed) PrdtEntry;

/*
 * Forward declaration
 */
typedef struct PataRequest PataRequest;

/*
 * Drive structure
 */
typedef struct {
    UINT16 BasePort;
    UINT16 CtrlPort;
    UINT8 Drive;
    PataChannel Channel;
    PataDeviceType Type;
    UINT64 TotalSectors;
    UINT16 SectorSize;
    INT SupportsLba48;
    volatile UINT8 IrqPending;
    volatile UINT8 IrqCount;
    UINT8 Irq;
    PciDevice* PciDev;
    UINT16 BusMasterBase;
    
    // NEW: Synchronization
    SpinLock DriveLock;
    volatile BOOL DmaInProgress;
    
    // NEW: Statistics
    UINT32 TotalReads;
    UINT32 TotalWrites;
    UINT32 DmaFailures;
    UINT32 PioFallbacks;
} PataDrive;

/*
 * Request structure
 */
struct PataRequest {
    struct PataRequest *Next;
    PataOperation Op;
    UINT64 Lba;
    UINT32 Count;
    VOID *Buffer;
    UINT32 Result;
    BOOL Completed;
    BOOL DmaActive;
    UINT32 TimeoutStart;
    PrdtEntry *Prdt;
    UINT16 PrdtPhys;
    SpinLock Lock;
};

/*
 * Public API
 */
INT PataInit(PataDrive *Drive, PataChannel Channel, UINT8 DriveNum);
INT PataIdentify(PataDrive *Drive, UINT16 IdentBuffer[256]);
INT PataReadSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, VOID *Buffer);
INT PataWriteSectors(PataDrive *Drive, UINT64 Lba, UINT32 Count, const VOID *Buffer);
INT PataFlushCache(PataDrive *Drive);

/*
 * Async API
 */
INT PataReadSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count, 
                         VOID *Buffer, PataRequest *Req);
INT PataWriteSectorsAsync(PataDrive *Drive, UINT64 Lba, UINT32 Count,
                          const VOID *Buffer, PataRequest *Req);
INT PataWaitRequest(PataRequest *Req, UINT32 TimeoutMs);
INT PataCancelRequest(PataRequest *Req);
BOOL PataIsRequestComplete(PataRequest *Req);

/*
 * IRQ handlers
 */
VOID PataPrimaryIrqHandler(VOID);
VOID PataSecondaryIrqHandler(VOID);
VOID PataEnableInterrupts(UINT16 CtrlPort);
VOID PataDisableInterrupts(UINT16 CtrlPort);

/*
 * Utility functions
 */
UINT8 PataGetIrqCount(PataDrive *Drive);
void PataResetStatistics(PataDrive *Drive);
