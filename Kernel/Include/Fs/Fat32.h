#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Fs/Vfs.h>
#include <Types.h>

// ============================================================================
// BPB (BIOS Parameter Block)
// ============================================================================

typedef struct {
    UINT8  JumpBoot[3];
    UINT8  OemName[8];
    UINT16 BytesPerSector;
    UINT8  SectorsPerCluster;
    UINT16 ReservedSectors;
    UINT8  NumFats;
    UINT16 RootEntries;
    UINT16 TotalSectors16;
    UINT8  MediaDescriptor;
    UINT16 FatSize16;
    UINT16 SectorsPerTrack;
    UINT16 NumHeads;
    UINT32 HiddenSectors;
    UINT32 TotalSectors32;

    // FAT32 specific
    UINT32 FatSize32;
    UINT16 ExtendedFlags;
    UINT16 FsVersion;
    UINT32 RootCluster;
    UINT16 FsInfo;
    UINT16 BackupBootSector;
    UINT8  Reserved[12];
    UINT8  DriveNumber;
    UINT8  Reserved1;
    UINT8  BootSignature;
    UINT32 VolumeId;
    UINT8  VolumeLabel[11];
    UINT8  FsType[8];
} ATTRIBUTE(packed) Fat32Bpb;

// ============================================================================
// FSInfo
// ============================================================================

typedef struct {
    UINT32 LeadSignature;
    UINT8  Reserved1[480];
    UINT32 StructSignature;
    UINT32 FreeCount;
    UINT32 NextFree;
    UINT8  Reserved2[12];
    UINT32 TrailSignature;
} ATTRIBUTE(packed) Fat32FsInfo;

// ============================================================================
// Directory entry
// ============================================================================

typedef struct {
    UINT8  Name[11];
    UINT8  Attr;
    UINT8  NtRes;
    UINT8  CrtTimeTenth;
    UINT16 CrtTime;
    UINT16 CrtDate;
    UINT16 LstAccDate;
    UINT16 FirstClusterHi;
    UINT16 WrtTime;
    UINT16 WrtDate;
    UINT16 FirstClusterLo;
    UINT32 FileSize;
} ATTRIBUTE(packed) Fat32DirEntry;

typedef struct {
    UINT8  Order;
    UINT16 Name1[5];
    UINT8  Attr;
    UINT8  Type;
    UINT8  Checksum;
    UINT16 Name2[6];
    UINT16 FirstCluster;
    UINT16 Name3[2];
} ATTRIBUTE(packed) Fat32LfnEntry;

// ============================================================================
// Attributes / FAT values
// ============================================================================

#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F

#define FAT32_EOC           0x0FFFFFF8
#define FAT32_BAD           0x0FFFFFF7
#define FAT32_FREE          0x00000000
#define FAT32_RESERVED      0x0FFFFFF0

#define FAT_DATE(Year, Month, Day)     ((((Year) - 1980) << 9) | ((Month) << 5) | (Day))
#define FAT_TIME(Hour, Minute, Second) (((Hour) << 11) | ((Minute) << 5) | ((Second) / 2))

// ============================================================================
// FAT32 context
// ============================================================================

typedef struct Fat32 {
    Disk *Dev;

    Fat32Bpb    Bpb;
    Fat32FsInfo FsInfo;

    UINT32 BytesPerSector;
    UINT32 SectorsPerCluster;
    UINT32 BytesPerCluster;
    UINT32 TotalClusters;

    UINT32 FatStart;
    UINT32 DataStart;
    UINT32 RootCluster;
    UINT32 FsinfoSector;

    UINT32 *FatCache;
    UINT32  FatEntries;

    INT Dirty;
} Fat32;

typedef struct {
    Fat32 *Fat;
    UINT32 FirstCluster;
    UINT32 DirCluster;
    UINT32 DirEntry;
    UINT32 ParentCluster;
    UINT32 EntryOffset;
} Fat32InodePrivate;

// ============================================================================
// Public API
// ============================================================================

VOID  Fat32Init(VOID);
INT   Fat32Format(Disk *Dev, UINT64 StartLba, UINT64 SectorCount);