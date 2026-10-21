#pragma once

#include <Scheduler.h>
#include <SpinLock.h>
#include <Types.h>

#define SATA_CMD_SLOTS          8

#define SATA_FIS_TYPE_REG_H2D   0x27
#define SATA_FIS_TYPE_REG_D2H   0x34
#define SATA_FIS_TYPE_DMA_ACT   0x39
#define SATA_FIS_TYPE_DMA_SETUP 0x41
#define SATA_FIS_TYPE_DATA      0x46
#define SATA_FIS_TYPE_BIST      0x58
#define SATA_FIS_TYPE_PIO_SETUP 0x5F
#define SATA_FIS_TYPE_DEV_BITS  0xA1

#define SATA_SIG_SATA           0x00000101
#define SATA_SIG_ATAPI          0xEB140101
#define SATA_SIG_SEMB           0xC33C0101
#define SATA_SIG_PM             0x96690101

#define SATA_GHC_HR             (1u << 0)
#define SATA_GHC_IE             (1u << 1)
#define SATA_GHC_AE             (1u << 31)
#define SATA_GHC_ENABLE         (1 << 31)

#define SATA_CAP_SSS            (1u << 27)
#define SATA_CAP_SALP           (1u << 26)
#define SATA_CAP_NCS_MASK       0x00001F00
#define SATA_CAP_NCS_SHIFT      8
#define SATA_CAP_NP             (0x1F)

#define SATA_CAP2_BOHC          (1u << 0)

#define SATA_BOHC_BIOS_BUSY     (1u << 4)
#define SATA_BOHC_OS_OWNERSHIP  (1u << 3)

#define SATA_HBA_PxCMD_ST       0x0001
#define SATA_HBA_PxCMD_SUD      0x0002
#define SATA_HBA_PxCMD_POD      0x0004
#define SATA_HBA_PxCMD_CLO      0x0008
#define SATA_HBA_PxCMD_FRE      0x0010
#define SATA_HBA_PxCMD_FR       0x4000
#define SATA_HBA_PxCMD_CR       0x8000
#define SATA_HBA_PxCMD_ASP      0x4000000
#define SATA_HBA_PxCMD_ICC       0xF0000000
#define SATA_HBA_PxCMD_ICC_MASK 0xF0000000
#define SATA_HBA_PxCMD_ICC_ACTIVE (1u << 28)
#define SATA_HBA_PORT_IPM_ACTIVE 1

#define SATA_HBA_PxIS_DHRS          (1u << 0)
#define SATA_HBA_PxIS_PSS           (1u << 1)
#define SATA_HBA_PxIS_DSS           (1u << 2)
#define SATA_HBA_PxIS_SDBS          (1u << 3)
#define SATA_HBA_PxIS_UFS           (1u << 4)
#define SATA_HBA_PxIS_DPS           (1u << 5)
#define SATA_HBA_PxIS_PCS           (1u << 6)
#define SATA_HBA_PxIS_DMPS          (1u << 7)
#define SATA_HBA_PxIS_PRCS          (1u << 22)
#define SATA_HBA_PxIS_IPMS          (1u << 23)
#define SATA_HBA_PxIS_OFS           (1u << 24)
#define SATA_HBA_PxIS_INFS          (1u << 26)
#define SATA_HBA_PxIS_IFS           (1u << 27)
#define SATA_HBA_PxIS_HBDS          (1u << 28)
#define SATA_HBA_PxIS_HBFS          (1u << 29)
#define SATA_HBA_PxIS_TFES          (1u << 30)
#define SATA_HBA_PxIS_CPDS          (1u << 31)

#define SATA_PXIE_DHRE          (1u << 0)
#define SATA_PXIE_PSE           (1u << 1)
#define SATA_PXIE_DSE           (1u << 2)
#define SATA_PXIE_SDBE          (1u << 3)
#define SATA_PXIE_UFE           (1u << 4)
#define SATA_PXIE_DPE           (1u << 5)
#define SATA_PXIE_PCE           (1u << 6)
#define SATA_PXIE_DMPE          (1u << 7)
#define SATA_PXIE_PRCE          (1u << 22)
#define SATA_PXIE_IPME          (1u << 23)
#define SATA_PXIE_OFE           (1u << 24)
#define SATA_PXIE_INFE          (1u << 26)
#define SATA_PXIE_IFE           (1u << 27)
#define SATA_PXIE_HBDE          (1u << 28)
#define SATA_PXIE_HBFE          (1u << 29)
#define SATA_PXIE_TFEE          (1u << 30)
#define SATA_PXIE_CPDE          (1u << 31)

#define SATA_HBA_PxSSTS_DET      0x0F
#define SATA_HBA_PxSSTS_DET_MASK 0x0F
#define SATA_HBA_PxSSTS_DET_PHY  1
#define SATA_HBA_PxSSTS_DET_PRESENT 3

#define SATA_SCTL_DET_MASK      0x0F
#define SATA_SCTL_DET_INIT      0x01
#define SATA_SCTL_PORT_IPM_NOPART    0x100
#define SATA_SCTL_PORT_IPM_NOSLUM    0x200
#define SATA_SCTL_PORT_IPM_NODSLP    0x400

#define SATA_ATA_CMD_READ_DMA_EX      0x25
#define SATA_ATA_CMD_WRITE_DMA_EX     0x35
#define SATA_ATA_CMD_IDENTIFY         0xEC
#define SATA_ATA_CMD_IDENTIFY_PACKET  0xA1   /* ATAPI */
#define SATA_ATA_CMD_PACKET           0xA0   /* ATAPI */
#define SATA_ATA_DEV_BUSY             0x80
#define SATA_ATA_DEV_DRQ              0x08
#define SATA_ATA_DEV_ERR              0x01

/* SCSI opcodes (for ATAPI) */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_REQUEST_SENSE      0x03
#define SCSI_INQUIRY            0x12
#define SCSI_START_STOP_UNIT    0x1B
#define SCSI_READ_CAPACITY_10   0x25
#define SCSI_READ_10            0x28
#define SCSI_READ_12            0xA8

typedef struct {
    UINT8 FisType;
    UINT8 PmPort : 4;
    UINT8 Rsv0 : 3;
    UINT8 C : 1;
    UINT8 Command;
    UINT8 FeatureL;
    UINT8 Lba0;
    UINT8 Lba1;
    UINT8 Lba2;
    UINT8 Device;
    UINT8 Lba3;
    UINT8 Lba4;
    UINT8 Lba5;
    UINT8 FeatureH;
    UINT8 CountL;
    UINT8 CountH;
    UINT8 Icc;
    UINT8 Control;
    UINT8 Rsv1[4];
} ATTRIBUTE(packed) FisRegH2D;

typedef struct {
    UINT8 FisType;
    UINT8 PmPort : 4;
    UINT8 Rsv0 : 2;
    UINT8 I : 1;
    UINT8 Rsv1 : 1;
    UINT8 Status;
    UINT8 Error;
    UINT8 Lba0;
    UINT8 Lba1;
    UINT8 Lba2;
    UINT8 Device;
    UINT8 Lba3;
    UINT8 Lba4;
    UINT8 Lba5;
    UINT8 Rsv2;
    UINT8 CountL;
    UINT8 CountH;
    UINT8 Rsv3[2];
    UINT8 Rsv4[4];
} ATTRIBUTE(packed) FisRegD2H;

typedef struct {
    UINT8 FisType;
    UINT8 PmPort : 4;
    UINT8 Rsv0 : 1;
    UINT8 D : 1;
    UINT8 I : 1;
    UINT8 Rsv1 : 1;
    UINT8 Status;
    UINT8 Error;
    UINT8 Lba0;
    UINT8 Lba1;
    UINT8 Lba2;
    UINT8 Device;
    UINT8 Lba3;
    UINT8 Lba4;
    UINT8 Lba5;
    UINT8 Rsv2;
    UINT8 CountL;
    UINT8 CountH;
    UINT8 Rsv3;
    UINT8 EStatus;
    UINT16 Tc;
    UINT8 Rsv4[2];
} ATTRIBUTE(packed) FisPioSetup;

typedef struct {
    UINT8 FisType;
    UINT8 PmPort : 4;
    UINT8 Rsv0 : 1;
    UINT8 D : 1;
    UINT8 I : 1;
    UINT8 A : 1;
    UINT8 RsvEd[2];
    UINT64 DmaBufferId;
    UINT32 RsvD;
    UINT32 DmaBufOffset;
    UINT32 TransferCount;
    UINT32 ResVd;
} ATTRIBUTE(packed) FisDmaSetup;

typedef volatile struct {
    UINT32 Clb;
    UINT32 Clbu;
    UINT32 Fb;
    UINT32 Fbu;
    UINT32 Is;
    UINT32 Ie;
    UINT32 Cmd;
    UINT32 Rsv0;
    UINT32 Tfd;
    UINT32 Sig;
    UINT32 Ssts;
    UINT32 Sctl;
    UINT32 Serr;
    UINT32 Sact;
    UINT32 Ci;
    UINT32 Sntf;
    UINT32 Fbs;
    UINT32 Rsv1[11];
    UINT32 Vendor[4];
} ATTRIBUTE(packed) HbaPort;

typedef volatile struct {
    UINT32 Cap;
    UINT32 Ghc;
    UINT32 Is;
    UINT32 Pi;
    UINT32 Vs;
    UINT32 CccCtl;
    UINT32 CccPts;
    UINT32 EmLoc;
    UINT32 EmCtl;
    UINT32 Cap2;
    UINT32 Bohc;
    UINT8 Rsv[0xA0 - 0x2C];
    UINT8 Vendor[0x100 - 0xA0];
    HbaPort Ports[32];
} ATTRIBUTE(packed) HbaMem;

typedef volatile struct {
    FisDmaSetup DsFis;
    UINT8 Pad0[4];
    FisPioSetup PsFis;
    UINT8 Pad1[12];
    FisRegD2H RFis;
    UINT8 Pad2[4];
    UINT8 SdbFis[8];
    UINT8 UFis[64];
    UINT8 Rsv[0x100 - 0xA0];
} ATTRIBUTE(packed) HbaFis;

typedef struct {
    UINT8 Cfl : 5;
    UINT8 A : 1;         /* ← ATAPI flag */
    UINT8 W : 1;
    UINT8 P : 1;
    UINT8 R : 1;
    UINT8 B : 1;
    UINT8 C : 1;
    UINT8 Rsv0 : 1;
    UINT8 Pmp : 4;
    UINT16 Prdtl;
    UINT32 Prdbc;
    UINT32 Ctba;
    UINT32 Ctbau;
    UINT32 Rsv1[4];
} ATTRIBUTE(packed) HbaCmdHeader;

typedef struct {
    UINT32 Dba;
    UINT32 DbaU;
    UINT32 Rsv0;
    UINT32 Dbc : 22;
    UINT32 Rsv1 : 9;
    UINT32 I : 1;
} ATTRIBUTE(packed) HbaPrdtEntry;

typedef struct {
    UINT8 CFis[64];
    UINT8 ACmd[16];      /* ← ATAPI Command */
    UINT8 Rsv[48];
    HbaPrdtEntry PrdtEntry[1];
} ATTRIBUTE(packed) HbaCmdTbl;

typedef enum {
    SATA_PORT_UNINITIALIZED = 0,
    SATA_PORT_ERROR = 1,
    SATA_PORT_ACTIVE = 2,
} SataPortStatus;

typedef struct {
    volatile BOOL Active;
    volatile BOOL Done;
    volatile INT Result;
    Task *Task;
    SpinLock SleepLock;
} SataSlotWait;

typedef struct SataPort {
    HbaPort *Regs;
    HbaCmdHeader *CmdList;
    HbaFis *Fis;
    HbaCmdTbl *CmdTables[SATA_CMD_SLOTS];
    UINT64 PhysBuffers[SATA_CMD_SLOTS];
    VOID *VirtBuffers[SATA_CMD_SLOTS];
    SataSlotWait SlotWait[SATA_CMD_SLOTS];
    volatile INT BufferSemaphore;
    volatile INT BufferLocks[8];
    volatile INT PortLock;
    SataPortStatus Status;
    UINT32 SectorSize;
    UINT64 TotalSectors;
    INT SupportsLba48;
    volatile UINT32 ErrorCount;
    INT PortNum;

    /* --- ATAPI --- */
    BOOL   IsAtapi;
    UINT32 Sig;
    UINT32 AtapiSectorSize;
    BOOL   AtapiMediaPresent;
    UINT8  AtapiSenseKey;
    UINT8  AtapiAsc;
    UINT8  AtapiAscq;
    CHAR   AtapiVendor[9];
    CHAR   AtapiProduct[17];
    CHAR   AtapiRevision[5];
} SataPort;

INT SataInit(VOID);
INT SataPortRead(SataPort *Port, UINT64 Lba, UINT32 Count, VOID *Buffer);
INT SataPortWrite(SataPort *Port, UINT64 Lba, UINT32 Count, const VOID *Buffer);
SataPort *SataGetPort(INT PortNum);
VOID SataIrqHandler(VOID);

/* ATAPI */
INT SataAtapiReadSectors(SataPort *Port, UINT64 Lba, UINT32 Count, VOID *Buffer);
INT SataAtapiTestUnitReady(SataPort *Port);
INT SataAtapiInquiry(SataPort *Port);
INT SataAtapiReadCapacity(SataPort *Port);
INT SataAtapiRequestSense(SataPort *Port);
INT SataAtapiRescan(SataPort *Port);