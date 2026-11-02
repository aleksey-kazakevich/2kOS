#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Fs/Vfs.h>
#include <Types.h>

/* ============================================================================ ISO9660 basic constants ============================================================================ */

#define ISO_SECTOR_SIZE           2048
#define ISO_PVD_LBA               16
#define ISO_PVD_ID                "CD001"
#define ISO_PVD_VERSION           1

/* Volume Descriptor Types */
#define ISO_VD_TYPE_BOOT          0x00
#define ISO_VD_TYPE_PRIMARY       0x01
#define ISO_VD_TYPE_SUPPLEMENTARY 0x02
#define ISO_VD_TYPE_PARTITION     0x03
#define ISO_VD_TYPE_TERMINATOR    0xFF

/* Joliet escape sequences (in SVD->SystemId[0..2]) */
#define ISO_JOLIET_ESC_UCS2_LEVEL1  "%/@"
#define ISO_JOLIET_ESC_UCS2_LEVEL2  "%/C"
#define ISO_JOLIET_ESC_UCS2_LEVEL3  "%/E"

/* Directory Record flags */
#define ISO_FLAG_HIDDEN           0x01
#define ISO_FLAG_DIRECTORY        0x02
#define ISO_FLAG_ASSOCIATED       0x04
#define ISO_FLAG_RECORD           0x08
#define ISO_FLAG_PROTECTION       0x10
#define ISO_FLAG_MULTI_EXTENT     0x80

/* ============================================================================ Rock Ridge (SUSP) ============================================================================ */

#define SUSP_ENTRY_SP             0x5350  /* "SP" — SUSP indicator */
#define SUSP_ENTRY_CE             0x4345  /* "CE" — Continuation Area */
#define SUSP_ENTRY_PD             0x5044  /* "PD" — Padding */
#define SUSP_ENTRY_ST             0x5354  /* "ST" — SUSP Terminator */

/* Rock Ridge (RRIP) */
#define RRIP_ENTRY_RR             0x5252  /* "RR" — RR flags */
#define RRIP_ENTRY_PX             0x5058  /* "PX" — POSIX attrs */
#define RRIP_ENTRY_NM             0x4E4D  /* "NM" — Alternative Name */
#define RRIP_ENTRY_SL             0x534C  /* "SL" — Symbolic Link */
#define RRIP_ENTRY_CL             0x434C  /* "CL" — Child Link */
#define RRIP_ENTRY_PL             0x504C  /* "PL" — Parent Link */
#define RRIP_ENTRY_RE             0x5245  /* "RE" — Relocated Directory */
#define RRIP_ENTRY_TF             0x5446  /* "TF" — Time Stamp */
#define RRIP_ENTRY_SF             0x5346  /* "SF" — Sparse File */
#define RRIP_ENTRY_ER             0x4552  /* "ER" — Extensions Reference */

/* RR flags (from RRIP_ENTRY_RR) */
#define RR_FLAG_PX                0x01
#define RR_FLAG_PN                0x02
#define RR_FLAG_SL                0x04
#define RR_FLAG_NM                0x08
#define RR_FLAG_CL                0x10
#define RR_FLAG_PL                0x20
#define RR_FLAG_RE                0x40
#define RR_FLAG_TF                0x80

/* NM flags */
#define NM_FLAG_CONTINUE          0x01

/* ============================================================================ El Torito (Boot Record) ============================================================================ */

#define EL_TORITO_ID              "EL TORITO SPECIFICATION"
#define EL_TORITO_BOOT_SIG        0x55AA

#define EL_TORITO_PLATFORM_X86    0x00
#define EL_TORITO_PLATFORM_PPC    0x01
#define EL_TORITO_PLATFORM_MAC    0x02
#define EL_TORITO_PLATFORM_EFI    0xEF

#define EL_TORITO_MEDIA_NO_EMU    0x00
#define EL_TORITO_MEDIA_12_FLOPPY 0x01
#define EL_TORITO_MEDIA_144_FLOPPY 0x02
#define EL_TORITO_MEDIA_288_FLOPPY 0x03
#define EL_TORITO_MEDIA_HDD       0x04

/* ============================================================================ Structures ============================================================================ */

typedef struct {
    UINT8  Type;
    UINT8  Identifier[5];
    UINT8  Version;
    UINT8  Unused1;
    UINT8  SystemId[32];
    UINT8  VolumeId[32];
    UINT8  Unused2[8];
    UINT32 VolumeSpaceSizeLE;
    UINT32 VolumeSpaceSizeBE;
    UINT8  Unused3[32];
    UINT16 VolumeSetSizeLE;
    UINT16 VolumeSetSizeBE;
    UINT16 VolumeSeqNumLE;
    UINT16 VolumeSeqNumBE;
    UINT16 LogicalBlockSizeLE;
    UINT16 LogicalBlockSizeBE;
    UINT32 PathTableSizeLE;
    UINT32 PathTableSizeBE;
    UINT32 PathTableLbaLE;
    UINT32 OptPathTableLbaLE;
    UINT32 PathTableLbaBE;
    UINT32 OptPathTableLbaBE;
    UINT8  RootDirRecord[34];
    UINT8  VolumeSetId[128];
    UINT8  PublisherId[128];
    UINT8  DataPreparerId[128];
    UINT8  ApplicationId[128];
    UINT8  CopyrightFileId[37];
    UINT8  AbstractFileId[37];
    UINT8  BibliographicFileId[37];
    UINT8  CreationDate[17];
    UINT8  ModificationDate[17];
    UINT8  ExpirationDate[17];
    UINT8  EffectiveDate[17];
    UINT8  FileStructureVersion;
    UINT8  Unused4;
    UINT8  ApplicationUse[512];
    UINT8  Reserved[653];
} ATTRIBUTE(packed) IsoPvd;

typedef struct {
    UINT8  Length;
    UINT8  ExtAttrLength;
    UINT32 ExtentLbaLE;
    UINT32 ExtentLbaBE;
    UINT32 DataLengthLE;
    UINT32 DataLengthBE;
    UINT8  RecordingDate[7];
    UINT8  Flags;
    UINT8  FileUnitSize;
    UINT8  InterleaveGap;
    UINT16 VolumeSeqNumLE;
    UINT16 VolumeSeqNumBE;
    UINT8  FileIdentifierLength;
    UINT8  FileIdentifier[1];
} ATTRIBUTE(packed) IsoDirRecord;

/* SUSP/Rock Ridge entry header — 4 bytes */
typedef struct {
    UINT8  Signature[2];
    UINT8  Length;
    UINT8  Version;
} ATTRIBUTE(packed) SuspEntryHeader;

/* El Torito Boot Record (volume descriptor type 0x00, id "EL TORITO SPEC") */
typedef struct {
    UINT8  Type;
    UINT8  Identifier[5];
    UINT8  Version;
    UINT8  BootSystemId[32];
    UINT8  BootId[32];
    UINT8  Unused[32];
    UINT32 BootCatalogLbaLE;
    UINT32 BootCatalogLbaBE;
    UINT8  Reserved[1977];
} ATTRIBUTE(packed) ElToritoDescriptor;

/* El Torito Boot Catalog — Default Entry (always the first entry) */
typedef struct {
    UINT8  BootIndicator;    /* 0x88 = bootable */
    UINT8  BootMediaType;    /* 0 = no emul, 1 = 1.2M, 2 = 1.44M, 3 = 2.88M, 4 = HDD */
    UINT16 LoadSegment;      /* 0000 for no emul */
    UINT8  SystemType;
    UINT8  Unused1;
    UINT16 SectorCount;      /* in 512-byte sectors */
    UINT32 LoadLbaLE;
    UINT8  Unused2[20];
} ATTRIBUTE(packed) ElToritoBootEntry;

/* ============================================================================ Context ============================================================================ */

typedef struct IsoContext {
    Disk   *Disk;
    UINT64  StartLba;
    UINT32  SectorSize;
    UINT64  TotalSectors;

    /* Primary VD */
    IsoPvd  Pvd;
    UINT32  RootLba;
    UINT32  RootSize;

    /* Supplementary VD (Joliet) */
    IsoPvd  Svd;
    BOOL    HasJoliet;
    UINT32  JolietRootLba;
    UINT32  JolietRootSize;

    /* Rock Ridge */
    BOOL    HasRockRidge;
    BOOL    HasSusp;
    UINT8   SuspSkipBytes;    /* how many bytes to skip at the beginning of SUA (SP) */

    /* El Torito */
    BOOL    HasElTorito;
    UINT32  BootCatalogLba;

    struct VfsInode *MountRoot;
} IsoContext;

typedef struct {
    IsoContext *Ctx;
    UINT32      ExtentLba;
    UINT32      DataLength;
    UINT32      ParentExtentLba;
    UINT8       Flags;

    /* Name source */
    BOOL        FromJoliet;
    BOOL        FromRockRidge;

    /* Metadata (if available) */
    BOOL        HasPosix;
    UINT32      PosixMode;
    UINT32      PosixUid;
    UINT32      PosixGid;
    UINT32      PosixNlink;

    CHAR        Name[256];
} IsoInodePrivate;

/* ============================================================================ Public API ============================================================================ */

INT Iso9660Init(VOID);
INT Iso9660Mount(Disk *Disk, UINT64 StartLba, UINT64 SectorCount, VfsInode **Root);

/* Utility for reading El Torito boot image (useful for kexec, etc.) */
INT Iso9660GetBootImage(IsoContext *Ctx, UINT32 *OutLba, UINT32 *OutSize);