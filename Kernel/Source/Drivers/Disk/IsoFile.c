#include <Drivers/Disk/IsoFile.h>
#include <Drivers/Disk/DiskMgr.h>
#include <Mem/Allocator.h>
#include <Io/StdWarn.h>
#include <Fs/Iso9660.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Io/StdOut.h>
#include <Io/StdErr.h>
#include <Return.h>

/* ISO9660 always uses 2048-byte logical sectors. */
#define ISO_FILE_SECTOR_SIZE   2048ULL

/* Chunk size used when the caller asks for a large read. Must be a
 * multiple of ISO_FILE_SECTOR_SIZE so that VfsRead stays sector-aligned. */
#define ISO_FILE_MAX_CHUNK     (64ULL * 1024ULL)

typedef struct {
    VfsFile *File;         /* open for the entire lifetime of the Disk */
    UINT64   SizeBytes;
    UINT64   SectorCount;
} IsoFileDisk;

typedef struct {
    const UINT8 *Data;      /* owned by the caller, never freed here */
    UINT64       SizeBytes;
    UINT64       SectorCount;
} IsoMemDisk;

/* ----------------------------------------------------------------------------
 * Disk->Read
 * -------------------------------------------------------------------------- */

STATIC INT IsoFileRead(Disk *Self, UINT64 Lba, UINT32 Count, VOID *Buf) {
    if (!Self || !Self->DriverData || !Buf) return DEVICE_INVALID;

    IsoFileDisk *B = (IsoFileDisk *)Self->DriverData;
    if (!B->File) return DEVICE_INVALID;

    UINT64 ByteOff = Lba * ISO_FILE_SECTOR_SIZE;
    UINT64 Bytes   = (UINT64)Count * ISO_FILE_SECTOR_SIZE;

    if (Count == 0) return SUCCESS;
    if (ByteOff + Bytes > B->SizeBytes) return INCORRECT_VALUE;

    /* VfsRead accepts UINT32 sizes; split if necessary. */
    UINT8 *Out = (UINT8 *)Buf;
    UINT64 Remaining = Bytes;

    while (Remaining > 0) {
        UINT32 This = (Remaining > ISO_FILE_MAX_CHUNK)
                    ? (UINT32)ISO_FILE_MAX_CHUNK
                    : (UINT32)Remaining;

        if (VfsSeek(B->File, ByteOff, SEEK_SET) != SUCCESS) return IO_ERROR;

        UINT32 Done = 0;
        INT Rc = VfsRead(B->File, Out, This, &Done);
        if (Rc != SUCCESS || Done != This) return IO_ERROR;

        Out       += This;
        ByteOff   += This;
        Remaining -= This;
    }

    return SUCCESS;
}

/* ----------------------------------------------------------------------------
 * Disk->Write — ISO images are read-only
 * -------------------------------------------------------------------------- */

STATIC INT IsoFileWrite(Disk *Self, UINT64 Lba, UINT32 Count, const VOID *Buf) {
    (VOID)Self; (VOID)Lba; (VOID)Count; (VOID)Buf;
    return NOT_SUPPORTED;
}

/* ----------------------------------------------------------------------------
 * Disk->Flush — no-op
 * -------------------------------------------------------------------------- */

STATIC INT IsoFileFlush(Disk *Self) {
    (VOID)Self;
    return SUCCESS;
}

/* ----------------------------------------------------------------------------
 * Disk->Destroy — called by DiskMgrUnregisterDisk()
 * -------------------------------------------------------------------------- */

STATIC VOID IsoFileDestroy(Disk *D) {
    if (!D) return;

    IsoFileDisk *B = (IsoFileDisk *)D->DriverData;
    if (B) {
        if (B->File) VfsClose(B->File);
        MemoryFree(B);
        D->DriverData = NULLPTR;
    }

    MemoryFree(D);
}

/* ----------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

STATIC VOID IsoFileBasename(const CHAR *Path, CHAR *Out, USIZE OutSz) {
    if (!Path || !Out || OutSz == 0) return;

    const CHAR *Base = Path;
    const CHAR *P = Path;
    while (*P) {
        if (*P == '/' || *P == '\\') Base = P + 1;
        P++;
    }

    /* Strip a trailing ".iso" if present, so "x.iso" becomes "x". */
    USIZE Len = StrLen(Base);
    if (Len > 4) {
        const CHAR *Ext = Base + Len - 4;
        if (StrCaseCmp(Ext, ".iso") == 0) Len -= 4;
    }

    if (Len == 0) {
        /* Fallback so that we never end up with an empty mount name. */
        StrnCpy(Out, "iso", OutSz - 1);
        Out[OutSz - 1] = '\0';
        return;
    }
    if (Len >= OutSz) Len = OutSz - 1;

    for (USIZE I = 0; I < Len; I++) Out[I] = Base[I];
    Out[Len] = '\0';
}

/* ----------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

INT IsoMountFromFile(VfsInode *BaseDir,
                     const CHAR *FilePath,
                     const CHAR *MountName) {
    if (!FilePath || !FilePath[0]) return INCORRECT_VALUE;

    if (!BaseDir) BaseDir = VfsCurrentDir;
    if (!BaseDir) return NO_OBJECT;

    /* 1. Open the image. */
    VfsFile *F = NULLPTR;
    INT Rc = VfsOpen(BaseDir, FilePath, VFS_O_READ, &F);
    if (Rc != SUCCESS) {
        StdErrPrintf("isofile: cannot open '%s' (%d)\n", FilePath, Rc);
        return Rc;
    }

    UINT64 Size = (F->FInode) ? F->FInode->ISize : 0;
    if (Size < 16 * ISO_FILE_SECTOR_SIZE) {
        StdErrPrintf("isofile: '%s' is too small to be an ISO\n", FilePath);
        VfsClose(F);
        return INCORRECT_VALUE;
    }

    /* Round up: the driver may legitimately read a full last sector. */
    Size = (Size + ISO_FILE_SECTOR_SIZE - 1) & ~(ISO_FILE_SECTOR_SIZE - 1);

    /* 2. Determine the mount point name. */
    CHAR Name[DRIVE_NAME_MAX];
    if (MountName && MountName[0]) {
        StrnCpy(Name, MountName, DRIVE_NAME_MAX - 1);
        Name[DRIVE_NAME_MAX - 1] = '\0';
    } else {
        IsoFileBasename(FilePath, Name, sizeof(Name));
    }

    /* 3. Reject duplicate mount names early, before allocating anything. */
    {
        CHAR Probe[DRIVE_NAME_MAX + 4];
        SnPrintf(Probe, sizeof(Probe), "%s::/", Name);
        VfsInode *Tmp = NULLPTR;
        if (VfsWalk(VfsRootInode, Probe, &Tmp) == 0) {
            VfsInodeUnref(Tmp);
            StdWarnPrintf("isofile: '%s' is already mounted\n", Name);
            VfsClose(F);
            return ALREADY_EXISTS;
        }
    }

    /* 4. Allocate the backing structure. */
    IsoFileDisk *B = (IsoFileDisk *)MemoryAllocate(sizeof(IsoFileDisk));
    if (!B) {
        VfsClose(F);
        return NO_MEMORY;
    }
    MemSet(B, 0, sizeof(*B));
    B->File        = F;
    B->SizeBytes   = Size;
    B->SectorCount = Size / ISO_FILE_SECTOR_SIZE;

    /* 5. Allocate the pseudo-Disk. */
    Disk *D = (Disk *)MemoryAllocate(sizeof(Disk));
    if (!D) {
        MemoryFree(B);
        VfsClose(F);
        return NO_MEMORY;
    }
    MemSet(D, 0, sizeof(D));

    StrnCpy(D->Name, Name, sizeof(D->Name) - 1);
    D->Type        = DISK_TYPE_OPTICAL;
    D->SectorSize  = (UINT32)ISO_FILE_SECTOR_SIZE;
    D->SectorCount = B->SectorCount;
    D->TotalSize   = B->SizeBytes;
    D->DriverData  = B;
    D->Initialized = TRUE;
    D->Read        = IsoFileRead;
    D->Write       = IsoFileWrite;
    D->Flush       = IsoFileFlush;
    D->IsVirtual   = TRUE;
    D->Destroy     = IsoFileDestroy;

    /* 6. Register with DiskMgr. This does NOT scan partitions
     *    (the disk type is OPTICAL, so PartitionScan is skipped). */
    Rc = DiskMgrRegisterDisk(D);
    if (Rc != SUCCESS) {
        StdErrPrintf("isofile: DiskMgrRegisterDisk failed (%d)\n", Rc);
        IsoFileDestroy(D);
        return Rc;
    }

    /* 7. Mount the ISO9660 filesystem under Name. */
    VfsInode *Root = NULLPTR;
    Rc = VfsMountEx("iso9660", D, 0, D->SectorCount, Name, &Root);
    if (Rc != SUCCESS) {
        StdErrPrintf("isofile: VfsMountEx(iso9660) failed (%d)\n", Rc);
        DiskMgrUnregisterDisk(D);   /* triggers IsoFileDestroy */
        return Rc;
    }

    StdOutPrintf("isofile: '%s' mounted as %s (%llu KB)\n",
                 FilePath, Name, B->SizeBytes / 1024);

    VfsInodeUnref(Root);
    return SUCCESS;
}

STATIC INT IsoMemRead(Disk *Self, UINT64 Lba, UINT32 Count, VOID *Buf) {
    if (!Self || !Self->DriverData || !Buf) return DEVICE_INVALID;

    IsoMemDisk *B = (IsoMemDisk *)Self->DriverData;
    if (!B->Data) return DEVICE_INVALID;

    UINT64 ByteOff = Lba * ISO_FILE_SECTOR_SIZE;
    UINT64 Bytes   = (UINT64)Count * ISO_FILE_SECTOR_SIZE;

    if (Count == 0) return SUCCESS;
    if (ByteOff + Bytes > B->SizeBytes) return INCORRECT_VALUE;

    MemCpy(Buf, B->Data + ByteOff, (USIZE)Bytes);
    return SUCCESS;
}

STATIC INT IsoMemWrite(Disk *Self, UINT64 Lba, UINT32 Count, const VOID *Buf) {
    (VOID)Self; (VOID)Lba; (VOID)Count; (VOID)Buf;
    return NOT_SUPPORTED;
}

STATIC INT IsoMemFlush(Disk *Self) {
    (VOID)Self;
    return SUCCESS;
}

STATIC VOID IsoMemDestroy(Disk *D) {
    if (!D) return;
    IsoMemDisk *B = (IsoMemDisk *)D->DriverData;
    if (B) MemoryFree(B);
    MemoryFree(D);
}

INT IsoMountFromMemory(const VOID *Data, UINT64 Size, const CHAR *Name) {
    if (!Data || Size < 16 * ISO_FILE_SECTOR_SIZE) return INCORRECT_VALUE;
    if (!Name || !Name[0]) return INCORRECT_VALUE;

    CHAR MountName[DRIVE_NAME_MAX];
    StrnCpy(MountName, Name, DRIVE_NAME_MAX - 1);
    MountName[DRIVE_NAME_MAX - 1] = '\0';

    /* Round up to a full sector so that the last read never overflows. */
    UINT64 Rounded = (Size + ISO_FILE_SECTOR_SIZE - 1) & ~(ISO_FILE_SECTOR_SIZE - 1);

    /* Refuse duplicate mount names. */
    {
        CHAR Probe[DRIVE_NAME_MAX + 4];
        SnPrintf(Probe, sizeof(Probe), "%s::/", MountName);
        VfsInode *Tmp = NULLPTR;
        if (VfsWalk(VfsRootInode, Probe, &Tmp) == 0) {
            VfsInodeUnref(Tmp);
            StdWarnPrintf("isomem: '%s' is already mounted\n", MountName);
            return ALREADY_EXISTS;
        }
    }

    IsoMemDisk *B = (IsoMemDisk *)MemoryAllocate(sizeof(IsoMemDisk));
    if (!B) return NO_MEMORY;
    MemSet(B, 0, sizeof(*B));
    B->Data        = (const UINT8 *)Data;
    B->SizeBytes   = Rounded;
    B->SectorCount = Rounded / ISO_FILE_SECTOR_SIZE;

    Disk *D = (Disk *)MemoryAllocate(sizeof(Disk));
    if (!D) { MemoryFree(B); return NO_MEMORY; }
    MemSet(D, 0, sizeof(D));

    StrnCpy(D->Name, MountName, sizeof(D->Name) - 1);
    D->Type        = DISK_TYPE_OPTICAL;
    D->SectorSize  = (UINT32)ISO_FILE_SECTOR_SIZE;
    D->SectorCount = B->SectorCount;
    D->TotalSize   = B->SizeBytes;
    D->DriverData  = B;
    D->Initialized = TRUE;
    D->Read        = IsoMemRead;
    D->Write       = IsoMemWrite;
    D->Flush       = IsoMemFlush;
    D->IsVirtual   = TRUE;
    D->Destroy     = IsoMemDestroy;

    INT Rc = DiskMgrRegisterDisk(D);
    if (Rc != SUCCESS) {
        IsoMemDestroy(D);
        return Rc;
    }

    VfsInode *Root = NULLPTR;
    Rc = VfsMountEx("iso9660", D, 0, D->SectorCount, MountName, &Root);
    if (Rc != SUCCESS) {
        StdErrPrintf("isomem: VfsMountEx(iso9660) failed (%d)\n", Rc);
        DiskMgrUnregisterDisk(D);
        return Rc;
    }

    StdOutPrintf("isomem: '%s' mounted (%llu KB)\n",
                 MountName, B->SizeBytes / 1024);
    VfsInodeUnref(Root);
    return SUCCESS;
}

INT IsoRegisterFromMemory(const VOID *Data, UINT64 Size, const CHAR *Name) {
    if (!Data || Size < 16 * ISO_FILE_SECTOR_SIZE) return INCORRECT_VALUE;
    if (!Name || !Name[0]) return INCORRECT_VALUE;

    CHAR DiskName[DRIVE_NAME_MAX];
    StrnCpy(DiskName, Name, DRIVE_NAME_MAX - 1);
    DiskName[DRIVE_NAME_MAX - 1] = '\0';

    /* Round up to a full sector. */
    UINT64 Rounded = (Size + ISO_FILE_SECTOR_SIZE - 1) & ~(ISO_FILE_SECTOR_SIZE - 1);

    /* Refuse duplicate names — DiskMgr would reject it anyway, but a
     * clear message is nicer. */
    if (DiskMgrFindDisk(DiskName)) {
        StdWarnPrintf("isomem: disk '%s' already registered\n", DiskName);
        return ALREADY_EXISTS;
    }

    IsoMemDisk *B = (IsoMemDisk *)MemoryAllocate(sizeof(IsoMemDisk));
    if (!B) return NO_MEMORY;
    MemSet(B, 0, sizeof(*B));
    B->Data        = (const UINT8 *)Data;
    B->SizeBytes   = Rounded;
    B->SectorCount = Rounded / ISO_FILE_SECTOR_SIZE;

    Disk *D = (Disk *)MemoryAllocate(sizeof(Disk));
    if (!D) { MemoryFree(B); return NO_MEMORY; }
    MemSet(D, 0, sizeof(D));

    StrnCpy(D->Name, DiskName, sizeof(D->Name) - 1);
    D->Type        = DISK_TYPE_OPTICAL;
    D->SectorSize  = (UINT32)ISO_FILE_SECTOR_SIZE;
    D->SectorCount = B->SectorCount;
    D->TotalSize   = B->SizeBytes;
    D->DriverData  = B;
    D->Initialized = TRUE;
    D->Read        = IsoMemRead;
    D->Write       = IsoMemWrite;
    D->Flush       = IsoMemFlush;
    D->IsVirtual   = TRUE;
    D->Destroy     = IsoMemDestroy;

    INT Rc = DiskMgrRegisterDisk(D);
    if (Rc != SUCCESS) {
        IsoMemDestroy(D);
        return Rc;
    }

    StdOutPrintf("isomem: '%s' registered (%llu KB)\n",
                 DiskName, B->SizeBytes / 1024);
    return SUCCESS;
}
