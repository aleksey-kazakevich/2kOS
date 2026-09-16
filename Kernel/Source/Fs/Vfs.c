#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Fs/Exfat.h>
#include <Basecon.h>
#include <Fs/Vfs.h>
#include <Return.h>

typedef struct DriveMount {
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR FsName[16];
    Disk *Dev;
    VfsInode *Root;
    struct DriveMount *Next;
} DriveMount;

STATIC FileSystem *GFsList = NULLPTR;
STATIC DriveMount *GDriveMounts = NULLPTR;
STATIC UINT64 GNextIno = 1;

VfsInode *VfsRootInode = NULLPTR;
VfsInode *VfsCurrentDir = NULLPTR;
CHAR VfsCurrentPath[PATH_MAX] = "/";
CHAR VfsCurrentDrive[DRIVE_NAME_MAX] = "";

STATIC INT VfsNamespaceLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result);
STATIC INT VfsNamespaceReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name,
                               UINT32 *NameLen, UINT32 *Type);
STATIC INT VfsNamespaceGetName(VfsInode *Inode, CHAR *Name, INT MaxLen);

STATIC VfsOperations GNamespaceOps = {
    .Lookup = VfsNamespaceLookup,
    .ReadDir = VfsNamespaceReadDir,
    .GetName = VfsNamespaceGetName,
};

STATIC VOID VfsCopyName(CHAR *Dst, const CHAR *Src, USIZE Max) {
    if (!Dst || Max == 0)
        return;
    if (!Src) {
        Dst[0] = '\0';
        return;
    }
    StrnCpy(Dst, Src, Max - 1);
    Dst[Max - 1] = '\0';
}

STATIC DriveMount *VfsFindDriveMount(const CHAR *Name) {
    if (!Name || !Name[0])
        return NULLPTR;
    for (DriveMount *M = GDriveMounts; M; M = M->Next) {
        if (StrCaseCmp(M->DriveName, Name) == 0)
            return M;
    }
    return NULLPTR;
}

STATIC DriveMount *VfsFindDriveMountByDev(Disk *Dev) {
    if (!Dev)
        return NULLPTR;
    for (DriveMount *M = GDriveMounts; M; M = M->Next) {
        if (M->Dev == Dev)
            return M;
    }
    return NULLPTR;
}

STATIC DriveMount *VfsFindDriveMountByRoot(VfsInode *Inode) {
    if (!Inode)
        return NULLPTR;
    for (DriveMount *M = GDriveMounts; M; M = M->Next) {
        if (M->Root == Inode)
            return M;
    }
    return NULLPTR;
}

STATIC BOOL VfsIsNamespaceRoot(VfsInode *Inode) {
    return Inode && Inode == VfsRootInode;
}

STATIC DriveMount *VfsMountForInode(VfsInode *Inode) {
    DriveMount *M;

    if (!Inode || VfsIsNamespaceRoot(Inode))
        return NULLPTR;
    M = VfsFindDriveMountByRoot(Inode);
    if (M)
        return M;
    return VfsFindDriveMountByDev(Inode->IDev);
}

STATIC INT VfsNormalizeRelPath(const CHAR *Path, CHAR *Out, USIZE OutSize) {
    if (!Path || !Out || OutSize == 0)
        return -1;
    if (Path[0] == '\0') {
        StrCpy(Out, "/");
        return 0;
    }
    if (Path[0] == '/') {
        StrnCpy(Out, Path, OutSize - 1);
        Out[OutSize - 1] = '\0';
        return 0;
    }
    SnPrintf(Out, OutSize, "/%s", Path);
    return 0;
}

STATIC const CHAR *VfsDetectFilesystem(Disk *Dev) {
    UINT8 Sector[512];
    ExfatVbr *Exfat;

    if (!Dev || !Dev->Read)
        return NULLPTR;
    if (Dev->Read(Dev, 0, 1, Sector) != SUCCESS)
        return NULLPTR;
    if (Sector[510] != 0x55 || Sector[511] != 0xAA)
        return NULLPTR;

    Exfat = (ExfatVbr *)Sector;
    if (MemCmp(Exfat->FsName, "EXFAT   ", 8) == 0)
        return "exfat";
    return NULLPTR;
}

INT VfsParsePath(const CHAR *Path, CHAR *DriveOut, CHAR *RelOut, USIZE OutSize) {
    const CHAR *Sep;
    const CHAR *After;
    USIZE DriveLen;

    if (!Path || !DriveOut || !RelOut || OutSize == 0)
        return -1;
    DriveOut[0] = '\0';
    RelOut[0] = '\0';

    if (Path[0] == ':' && Path[1] == ':') {
        if (VfsCurrentDrive[0] == '\0')
            return -1;
        StrCpy(DriveOut, VfsCurrentDrive);
        After = Path + 2;
        if (After[0] == '\0') {
            StrCpy(RelOut, "/");
            return 0;
        }
        return VfsNormalizeRelPath(After, RelOut, OutSize);
    }

    Sep = StrStr(Path, "::");
    if (Sep) {
        DriveLen = (USIZE)(Sep - Path);
        if (DriveLen == 0 || DriveLen >= DRIVE_NAME_MAX)
            return -1;
        StrnCpy(DriveOut, Path, DriveLen);
        DriveOut[DriveLen] = '\0';
        After = Sep + 2;
        if (After[0] == '\0') {
            StrCpy(RelOut, "/");
            return 0;
        }
        return VfsNormalizeRelPath(After, RelOut, OutSize);
    }

    if (Path[0] == '/')
        return VfsNormalizeRelPath(Path, RelOut, OutSize);

    if (VfsCurrentDrive[0] == '\0')
        return -1;
    StrCpy(DriveOut, VfsCurrentDrive);
    return VfsNormalizeRelPath(Path, RelOut, OutSize);
}

STATIC INT VfsWalkWithin(VfsInode *Start, const CHAR *RelPath, VfsInode *Ceiling, VfsInode **Result) {
    VfsInode *Current;
    CHAR PathCopy[PATH_MAX];
    CHAR *SavePtr;
    CHAR *Components[256];
    INT CompCount;
    CHAR *Token;
    INT I;

    if (!Start || !RelPath || !Result)
        return -1;
    if (!Ceiling)
        Ceiling = Start;

    if (RelPath[0] == '\0' || (RelPath[0] == '/' && RelPath[1] == '\0')) {
        *Result = Start;
        VfsInodeRef(*Result);
        return 0;
    }

    StrnCpy(PathCopy, RelPath, PATH_MAX - 1);
    PathCopy[PATH_MAX - 1] = '\0';

    CompCount = 0;
    Token = StrTokR(PathCopy, "/", &SavePtr);
    while (Token && CompCount < 256) {
        Components[CompCount++] = Token;
        Token = StrTokR(NULLPTR, "/", &SavePtr);
    }

    Current = Start;
    VfsInodeRef(Current);

    for (I = 0; I < CompCount; I++) {
        CHAR *Comp = Components[I];
        VfsInode *Next;

        if (StrCmp(Comp, ".") == 0)
            continue;

        if (StrCmp(Comp, "..") == 0) {
            if (Current == Ceiling || VfsIsNamespaceRoot(Current))
                continue;
            if (VfsParent(Current, &Next) != 0) {
                VfsInodeUnref(Current);
                return -1;
            }
            VfsInodeUnref(Current);
            Current = Next;
            continue;
        }

        if (!Current->IOp || !Current->IOp->Lookup) {
            VfsInodeUnref(Current);
            return -1;
        }

        if (Current->IOp->Lookup(Current, Comp, &Next) != 0) {
            VfsInodeUnref(Current);
            return -1;
        }

        VfsInodeUnref(Current);
        Current = Next;
    }

    *Result = Current;
    return 0;
}

STATIC INT VfsNamespaceLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    DriveMount *M;

    (void)Dir;
    M = VfsFindDriveMount(Name);
    if (!M)
        return -1;
    *Result = M->Root;
    VfsInodeRef(*Result);
    return 0;
}

STATIC INT VfsNamespaceReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name,
                               UINT32 *NameLen, UINT32 *Type) {
    UINT64 Index = 0;

    (void)Dir;
    if (!Pos || !Name || !NameLen || !Type)
        return -1;

    for (DriveMount *M = GDriveMounts; M; M = M->Next, Index++) {
        if (Index == *Pos) {
            StrCpy(Name, M->DriveName);
            *NameLen = StrLen(Name);
            *Type = FT_DIR;
            (*Pos)++;
            return 0;
        }
    }
    return -1;
}

STATIC INT VfsNamespaceGetName(VfsInode *Inode, CHAR *Name, INT MaxLen) {
    (void)Inode;
    if (!Name || MaxLen <= 0)
        return -1;
    Name[0] = '\0';
    return 0;
}

INT VfsWalk(VfsInode *Dir, const CHAR *Path, VfsInode **Result) {
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR RelPath[PATH_MAX];
    DriveMount *M;

    if (!Path || !Result)
        return -1;
    if (!Dir)
        Dir = VfsCurrentDir;
    if (!Dir)
        return -1;

    if (Path[0] == '\0' || StrCmp(Path, ".") == 0) {
        *Result = Dir;
        VfsInodeRef(*Result);
        return 0;
    }

    if (StrCmp(Path, "/") == 0) {
        M = VfsMountForInode(VfsCurrentDir);
        if (M) {
            *Result = M->Root;
            VfsInodeRef(*Result);
            return 0;
        }
        *Result = VfsRootInode;
        VfsInodeRef(*Result);
        return 0;
    }

    if (StrStr(Path, "::") == NULLPTR && Path[0] != '/' &&
        !(Path[0] == ':' && Path[1] == ':') && StrChr(Path, '/') == NULLPTR) {
        M = VfsFindDriveMount(Path);
        if (M) {
            *Result = M->Root;
            VfsInodeRef(*Result);
            return 0;
        }
    }

    if (StrStr(Path, "::") != NULLPTR || (Path[0] == ':' && Path[1] == ':')) {
        if (VfsParsePath(Path, DriveName, RelPath, sizeof(RelPath)) != 0)
            return -1;
        M = VfsFindDriveMount(DriveName);
        if (!M)
            return -1;
        return VfsWalkWithin(M->Root, RelPath, M->Root, Result);
    }

    if (Path[0] == '/') {
        // Приоритет: mount, на котором находится Dir
        M = VfsMountForInode(Dir);
        if (!M)
            M = VfsMountForInode(VfsCurrentDir);
        
        // Если Dir — это namespace root (нет mount), используем первый mount
        if (!M && GDriveMounts)
            M = GDriveMounts;
        
        if (!M) {
            // Нет смонтированных дисков — работаем от namespace root
            return VfsWalkWithin(VfsRootInode, Path, VfsRootInode, Result);
        }
        
        // Идём от корня диска!
        return VfsWalkWithin(M->Root, Path, M->Root, Result);
    }

    return VfsWalkWithin(Dir, Path, VfsRootInode, Result);
}

INT VfsLookup(VfsInode *Dir, const CHAR *Name, VfsInode **Result) {
    if (!Dir || !Name || !Result)
        return -1;
    if (Dir->IMode != FT_DIR)
        return -1;
    if (!Dir->IOp || !Dir->IOp->Lookup)
        return -1;
    return Dir->IOp->Lookup(Dir, Name, Result);
}

STATIC VOID VfsStripTrailingSlashes(CHAR *Path) {
    USIZE Len;

    if (!Path)
        return;
    Len = StrLen(Path);
    while (Len > 1 && Path[Len - 1] == '/') {
        if (Len >= 2 && Path[Len - 2] == ':')
            break;
        Path[--Len] = '\0';
    }
}

INT VfsResolvePath(VfsInode *BaseDir, const CHAR *Path,
                   VfsInode **Parent, CHAR *Name, BOOL FollowLast) {
    CHAR FullPath[PATH_MAX];
    CHAR DriveName[DRIVE_NAME_MAX];
    CHAR RelPath[PATH_MAX];
    const CHAR *LastSlash;
    CHAR ParentRel[PATH_MAX];
    DriveMount *M;

    (VOID)FollowLast;
    if (!Path || !Parent || !Name) {
        return -1;
    }
    if (!BaseDir)
        BaseDir = VfsCurrentDir;
    if (!BaseDir) {
        return -1;
    }

    if (Path[0] == '\0' || (Path[0] == '/' && Path[1] == '\0')) {
        *Parent = VfsRootInode;
        VfsInodeRef(*Parent);
        Name[0] = '\0';
        return 0;
    }

    if (Path[0] != '/' && StrStr(Path, "::") == NULLPTR &&
        !(Path[0] == ':' && Path[1] == ':')) {
        M = VfsFindDriveMount(Path);
        if (M && StrChr(Path, '/') == NULLPTR) {
            *Parent = VfsRootInode;
            VfsInodeRef(*Parent);
            VfsCopyName(Name, Path, NAME_MAX);
            return 0;
        }
        
        INT BuildRet = VfsBuildPath(BaseDir, FullPath, sizeof(FullPath));
        if (BuildRet != 0) {
            return -1;
        }
        
        USIZE Len = StrLen(FullPath);
        if (Len > 0 && FullPath[Len - 1] != '/')
            SnPrintf(FullPath, sizeof(FullPath), "%s/%s", FullPath, Path);
        else
            SnPrintf(FullPath, sizeof(FullPath), "%s%s", FullPath, Path);
        
        Path = FullPath;
    }

    INT ParseRet = VfsParsePath(Path, DriveName, RelPath, sizeof(RelPath));
    if (ParseRet != 0) {
        return -1;
    }

    if (DriveName[0] == '\0') {
        *Parent = VfsRootInode;
        VfsInodeRef(*Parent);
        Name[0] = '\0';
        return 0;
    }

    M = VfsFindDriveMount(DriveName);
    if (!M) {
        return -1;
    }

    LastSlash = StrrChr(RelPath, '/');
    if (!LastSlash) {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        VfsCopyName(Name, RelPath, NAME_MAX);
        return 0;
    }

    if (LastSlash == RelPath && RelPath[1] == '\0') {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        Name[0] = '\0';
        return 0;
    }

    if (LastSlash == RelPath) {
        *Parent = M->Root;
        VfsInodeRef(*Parent);
        VfsCopyName(Name, LastSlash + 1, NAME_MAX);
        return 0;
    }

    StrnCpy(ParentRel, RelPath, (USIZE)(LastSlash - RelPath));
    ParentRel[LastSlash - RelPath] = '\0';
    
    INT WalkRet = VfsWalkWithin(M->Root, ParentRel, M->Root, Parent);
    if (WalkRet != 0) {
        return -1;
    }
    
    VfsCopyName(Name, LastSlash + 1, NAME_MAX);
    return 0;
}

INT VfsWalkParent(VfsInode *Dir, const CHAR *Path, VfsInode **Parent, CHAR *Name) {
    return VfsResolvePath(Dir, Path, Parent, Name, FALSE);
}

INT VfsRegisterFs(FileSystem *Fs) {
    if (!Fs)
        return -1;
    Fs->Next = GFsList;
    GFsList = Fs;
    return 0;
}

INT VfsMount(const CHAR *FsName, Disk *Dev, UINT64 StartLba, UINT64 SectorCount, VfsInode **Root) {
    FileSystem *Fs;
    const CHAR *RealFsName = NULLPTR;
    DriveMount *M;
    VfsInode *FsRoot;

    if (!Dev)
        return -1;

    if (VfsFindDriveMount(Dev->Name)) {
        BaseconPrintf(BASECON_TYPE_WARNING, "vfs: drive %s already mounted\n", Dev->Name);
        return -1;
    }

    if (!FsName || FsName[0] == '\0' || StrCaseCmp(FsName, "auto") == 0) {
        RealFsName = VfsDetectFilesystem(Dev);
        if (!RealFsName) {
            BaseconPrintf(BASECON_TYPE_ERROR, "vfs: cannot detect FS on %s\n", Dev->Name);
            return -1;
        }
        BaseconPrintf(BASECON_TYPE_NORMAL, "vfs: auto-detected FS: %s\n", RealFsName);
    } else {
        RealFsName = FsName;
    }

    for (Fs = GFsList; Fs; Fs = Fs->Next) {
        if (StrCaseCmp(Fs->Name, RealFsName) != 0)
            continue;

        if (Fs->Mount(Dev, StartLba, SectorCount, &FsRoot) != 0) {
            BaseconPrintf(BASECON_TYPE_ERROR, "vfs: mount failed for %s\n", Dev->Name);
            return -1;
        }

        M = (DriveMount *)MemoryAllocate(sizeof(DriveMount));
        if (!M) {
            if (Fs->Unmount)
                Fs->Unmount(FsRoot);
            VfsInodeUnref(FsRoot);
            return -1;
        }

        StrCpy(M->DriveName, Dev->Name);
        StrCpy(M->FsName, Fs->Name);
        M->Dev = Dev;
        M->Root = FsRoot;
        M->Next = GDriveMounts;
        GDriveMounts = M;

        if (Root) {
            VfsInodeRef(FsRoot);
            *Root = FsRoot;
        }

        BaseconPrintf(BASECON_TYPE_SUCCESS, "vfs: mounted %s on %s\n", Dev->Name, Fs->Name);
        VfsCd(&VfsCurrentDir, Dev->Name);
        return 0;
    }

    BaseconPrintf(BASECON_TYPE_ERROR, "vfs: FS %s not found\n", RealFsName);
    return -1;
}

INT VfsUnmount(VfsInode *Root) {
    if (!Root)
        return -1;
    for (DriveMount *M = GDriveMounts; M; M = M->Next) {
        if (M->Root == Root)
            return VfsUnmountDrive(M->DriveName);
    }
    return -1;
}

INT VfsUnmountDrive(const CHAR *DriveName) {
    DriveMount *M = VfsFindDriveMount(DriveName);
    DriveMount **Prev;
    Disk *Dev;

    if (!M)
        return -1;

    Dev = M->Dev;

    if (VfsCurrentDir && !VfsIsNamespaceRoot(VfsCurrentDir) &&
        VfsMountForInode(VfsCurrentDir) == M) {
        VfsInodeUnref(VfsCurrentDir);
        VfsCurrentDir = VfsRootInode;
        VfsInodeRef(VfsCurrentDir);
        VfsUpdateCurrentPath();
    }

    VfsSync(M->Root);
    if (M->Root->IOp && M->Root->IOp->Unmount)
        M->Root->IOp->Unmount(M->Root);

    VfsInodeUnref(M->Root);

    Prev = &GDriveMounts;
    while (*Prev) {
        if (*Prev == M) {
            *Prev = M->Next;
            break;
        }
        Prev = &(*Prev)->Next;
    }

    (void)Dev;
    MemoryFree(M);
    BaseconSetPrompt("2KOS> ");
    return 0;
}

INT VfsListMountedDrives(CHAR *Buf, USIZE BufSize) {
    USIZE Pos = 0;

    if (!Buf || BufSize == 0)
        return -1;
    Buf[0] = '\0';

    for (DriveMount *M = GDriveMounts; M; M = M->Next) {
        INT Written = SnPrintf(Buf + Pos, BufSize - Pos, "%s%s(%s)",
                               Pos ? " " : "", M->DriveName, M->FsName);
        if (Written <= 0 || (USIZE)Written >= BufSize - Pos)
            break;
        Pos += (USIZE)Written;
    }
    return (INT)Pos;
}

INT VfsOpen(VfsInode *Dir, const CHAR *Path, UINT32 Flags, VfsFile **File) {
    VfsInode *Inode = NULLPTR;
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    VfsFile *F;

    if (!Path || !File)
        return -1;

    if (VfsWalk(Dir, Path, &Inode) != 0) {
        if (!(Flags & O_CREAT))
            return -1;
        if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0)
            return -1;
        if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Create) {
            VfsInodeUnref(Parent);
            return -1;
        }
        if (Parent->IOp->Create(Parent, Name, FT_REG_FILE, &Inode) != 0) {
            VfsInodeUnref(Parent);
            return -1;
        }
        VfsInodeUnref(Parent);
    }

    if ((Flags & O_TRUNC) && Inode->IMode == FT_REG_FILE &&
        Inode->IFop && Inode->IFop->Truncate) {
        Inode->IFop->Truncate(Inode, 0);
        Inode->ISize = 0;
    }

    F = (VfsFile *)MemoryAllocate(sizeof(VfsFile));
    if (!F) {
        VfsInodeUnref(Inode);
        return -1;
    }

    F->FInode = Inode;
    F->FPos = (Flags & O_APPEND) ? Inode->ISize : 0;
    F->FFlags = Flags;
    F->FPrivate = NULLPTR;
    *File = F;
    return 0;
}

INT VfsClose(VfsFile *File) {
    if (!File)
        return -1;
    if (File->FInode && File->FInode->IDirty)
        VfsSync(File->FInode);
    if (File->FInode)
        VfsInodeUnref(File->FInode);
    MemoryFree(File);
    return 0;
}

INT VfsRead(VfsFile *File, VOID *Buf, UINT32 Size, UINT32 *Read) {
    INT Ret;

    if (!File || !File->FInode || !File->FInode->IFop || !File->FInode->IFop->Read)
        return -1;
    if ((File->FFlags & (O_READ | O_WRITE)) == O_WRITE)
        return -1;

    Ret = File->FInode->IFop->Read(File->FInode, File->FPos, Buf, Size, Read);
    if (Ret == 0 && Read)
        File->FPos += *Read;
    return Ret;
}

INT VfsWrite(VfsFile *File, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    INT Ret;

    if (!File || !File->FInode || !File->FInode->IFop || !File->FInode->IFop->Write)
        return -1;
    if (File->FFlags && !(File->FFlags & O_WRITE))
        return -1;

    Ret = File->FInode->IFop->Write(File->FInode, File->FPos, Buf, Size, Written);
    if (Ret == 0) {
        if (Written)
            File->FPos += *Written;
        if (File->FPos > File->FInode->ISize)
            File->FInode->ISize = File->FPos;
        File->FInode->IDirty = 1;
    }
    return Ret;
}

INT VfsSeek(VfsFile *File, UINT64 Offset, INT Whence) {
    if (!File || !File->FInode)
        return -1;
    switch (Whence) {
    case SEEK_SET:
        File->FPos = Offset;
        break;
    case SEEK_CUR:
        File->FPos += Offset;
        break;
    case SEEK_END:
        File->FPos = File->FInode->ISize + Offset;
        break;
    default:
        return -1;
    }
    return 0;
}

INT VfsMkdir(VfsInode *Dir, const CHAR *Path, UINT32 Mode, VfsInode **Result) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    INT Ret;

    if (!Path)
        return -1;
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0)
        return -1;
    if (Name[0] == '\0' || Parent->IMode != FT_DIR || !Parent->IOp || !Parent->IOp->Mkdir) {
        VfsInodeUnref(Parent);
        return -1;
    }

    Ret = Parent->IOp->Mkdir(Parent, Name, Mode ? Mode : FT_DIR, Result);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsRmdir(VfsInode *Dir, const CHAR *Path) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    INT Ret;

    if (!Path)
        return -1;
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0)
        return -1;
    if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Rmdir) {
        VfsInodeUnref(Parent);
        return -1;
    }

    Ret = Parent->IOp->Rmdir(Parent, Name);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsReadDir(VfsInode *Dir, UINT64 *Pos, CHAR *Name, UINT32 *NameLen, UINT32 *Type) {
    if (!Dir || !Pos || !Name || !NameLen || !Type)
        return -1;
    if (Dir->IMode != FT_DIR)
        return -1;
    if (!Dir->IOp || !Dir->IOp->ReadDir)
        return -1;
    return Dir->IOp->ReadDir(Dir, Pos, Name, NameLen, Type);
}

INT VfsCreate(VfsInode *Dir, const CHAR *Path, UINT32 Mode, VfsInode **Result) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    INT Ret;

    if (!Path)
        return -1;
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0)
        return -1;
    if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Create) {
        VfsInodeUnref(Parent);
        return -1;
    }

    Ret = Parent->IOp->Create(Parent, Name, Mode, Result);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsUnlink(VfsInode *Dir, const CHAR *Path) {
    VfsInode *Parent;
    CHAR Name[NAME_MAX];
    INT Ret;

    if (!Path)
        return -1;
    if (VfsResolvePath(Dir, Path, &Parent, Name, FALSE) != 0)
        return -1;
    if (Name[0] == '\0' || !Parent->IOp || !Parent->IOp->Unlink) {
        VfsInodeUnref(Parent);
        return -1;
    }

    Ret = Parent->IOp->Unlink(Parent, Name);
    VfsInodeUnref(Parent);
    return Ret;
}

INT VfsRename(VfsInode *OldDir, const CHAR *OldPath,
              VfsInode *NewDir, const CHAR *NewPath) {
    VfsInode *OldParent, *NewParent;
    CHAR OldName[NAME_MAX], NewName[NAME_MAX];
    INT Ret;

    if (!OldPath || !NewPath)
        return -1;
    if (VfsResolvePath(OldDir, OldPath, &OldParent, OldName, FALSE) != 0)
        return -1;
    if (VfsResolvePath(NewDir, NewPath, &NewParent, NewName, FALSE) != 0) {
        VfsInodeUnref(OldParent);
        return -1;
    }
    if (OldName[0] == '\0' || NewName[0] == '\0' || !OldParent->IOp || !OldParent->IOp->Rename) {
        VfsInodeUnref(OldParent);
        VfsInodeUnref(NewParent);
        return -1;
    }

    Ret = OldParent->IOp->Rename(OldParent, OldName, NewParent, NewName);
    VfsInodeUnref(OldParent);
    VfsInodeUnref(NewParent);
    return Ret;
}

INT VfsStat(VfsInode *Inode, VfsStats *Stat) {
    if (!Inode || !Stat)
        return -1;
    Stat->StMode = Inode->IMode;
    Stat->StUid = Inode->IUid;
    Stat->StGid = Inode->IGid;
    Stat->StSize = Inode->ISize;
    Stat->StCtime = Inode->ICtime;
    Stat->StMtime = Inode->IMtime;
    Stat->StAtime = Inode->IAtime;
    Stat->StIno = Inode->IIno;
    Stat->StNlink = Inode->INlink;
    return 0;
}

INT VfsChmod(VfsInode *Inode, UINT32 Mode) {
    if (!Inode || !Inode->IOp || !Inode->IOp->Chmod)
        return -1;
    return Inode->IOp->Chmod(Inode, Mode);
}

INT VfsSync(VfsInode *Inode) {
    INT Ret;

    if (!Inode || !Inode->IFop || !Inode->IFop->Sync)
        return -1;
    Ret = Inode->IFop->Sync(Inode);
    if (Ret == 0)
        Inode->IDirty = 0;
    return Ret;
}

INT VfsChown(VfsInode *Inode, UINT32 Uid, UINT32 Gid) {
    if (!Inode)
        return -1;
    Inode->IUid = Uid;
    Inode->IGid = Gid;
    return 0;
}

VfsInode *VfsAllocInode(VOID) {
    VfsInode *Inode = (VfsInode *)MemoryAllocate(sizeof(VfsInode));
    if (Inode) {
        MemSet(Inode, 0, sizeof(VfsInode));
        Inode->IRefCount = 1;
        Inode->IIno = GNextIno++;
    }
    return Inode;
}

VOID VfsInodeRef(VfsInode *Inode) {
    if (Inode)
        Inode->IRefCount++;
}

VOID VfsInodeUnref(VfsInode *Inode) {
    if (!Inode)
        return;
    if (Inode->IRefCount == 0)
        return;
    if (--Inode->IRefCount == 0) {
        if (Inode->IOp && Inode->IOp->Destroy)
            Inode->IOp->Destroy(Inode);
        else if (Inode->IPrivate)
            MemoryFree(Inode->IPrivate);
        MemoryFree(Inode);
    }
}

VOID VfsFreeInode(VfsInode *Inode) {
    VfsInodeUnref(Inode);
}

INT VfsParent(VfsInode *Inode, VfsInode **Parent) {
    INT Ret;

    if (!Inode || !Parent)
        return -1;
    if (VfsIsNamespaceRoot(Inode)) {
        *Parent = VfsRootInode;
        VfsInodeRef(*Parent);
        return 0;
    }
    if (VfsFindDriveMountByRoot(Inode)) {
        *Parent = VfsRootInode;
        VfsInodeRef(*Parent);
        return 0;
    }
    if (Inode->IOp && Inode->IOp->Parent) {
        Ret = Inode->IOp->Parent(Inode, Parent);
        return Ret;
    }
    return -1;
}

INT VfsCd(VfsInode **Dir, const CHAR *Path) {
    VfsInode *NewDir;

    if (!Dir || !Path)
        return -1;
    if (VfsWalk(*Dir, Path, &NewDir) != 0)
        return -1;
    if (NewDir->IMode != FT_DIR) {
        VfsInodeUnref(NewDir);
        return -1;
    }
    VfsInodeUnref(*Dir);
    *Dir = NewDir;
    VfsUpdateCurrentPath();
    return 0;
}

INT VfsBuildPath(VfsInode *Inode, CHAR *Buffer, INT MaxLen) {
    DriveMount *M;
    VfsInode *Current;
    VfsInode *Parent;
    CHAR Components[256][NAME_MAX];
    INT CompCount = 0;
    INT Pos;
    INT I;

    if (!Inode || !Buffer || MaxLen <= 0)
        return -1;
    if (VfsIsNamespaceRoot(Inode)) {
        StrCpy(Buffer, "/");
        return 0;
    }

    M = VfsMountForInode(Inode);
    if (!M)
        return -1;

    VfsInodeRef(Inode);
    Current = Inode;

    while (Current && Current != M->Root && CompCount < 256) {
        CHAR Comp[NAME_MAX];

        if (!Current->IOp || !Current->IOp->GetName) {
            VfsInodeUnref(Current);
            return -1;
        }
        if (Current->IOp->GetName(Current, Comp, NAME_MAX) != 0) {
            VfsInodeUnref(Current);
            return -1;
        }
        if (Comp[0] != '\0') {
            StrnCpy(Components[CompCount], Comp, NAME_MAX - 1);
            Components[CompCount][NAME_MAX - 1] = '\0';
            CompCount++;
        }
        if (VfsParent(Current, &Parent) != 0) {
            VfsInodeUnref(Current);
            return -1;
        }
        VfsInodeUnref(Current);
        Current = Parent;
    }

    if (!Current || Current != M->Root) {
        if (Current)
            VfsInodeUnref(Current);
        return -1;
    }
    VfsInodeUnref(Current);

    Pos = SnPrintf(Buffer, (USIZE)MaxLen, "%s::/", M->DriveName);
    if (Pos < 0 || Pos >= MaxLen)
        return -1;
    for (I = CompCount - 1; I >= 0; I--) {
        INT Len = StrLen(Components[I]);
        if (Pos + Len + (Pos > 0 && Buffer[Pos - 1] != '/' ? 1 : 0) >= MaxLen)
            return -1;
        if (Buffer[Pos - 1] != '/')
            Buffer[Pos++] = '/';
        StrCpy(Buffer + Pos, Components[I]);
        Pos += Len;
    }
    Buffer[Pos] = '\0';
    return 0;
}

VOID VfsUpdateCurrentPath(VOID) {
    CHAR Prompt[256];
    const CHAR *DisplayPath;
    const CHAR *Sep;
    DriveMount *M;
    USIZE Len;

    if (!VfsRootInode || !VfsCurrentDir) {
        StrCpy(VfsCurrentPath, "/");
        VfsCurrentDrive[0] = '\0';
        return;
    }

    if (VfsIsNamespaceRoot(VfsCurrentDir)) {
        StrCpy(VfsCurrentPath, "/");
        VfsCurrentDrive[0] = '\0';
        SnPrintf(Prompt, sizeof(Prompt), "/> ");
        BaseconSetPrompt(Prompt);
        BaseconRedrawFull();
        return;
    }

    if (VfsBuildPath(VfsCurrentDir, VfsCurrentPath, PATH_MAX) != 0) {
        StrCpy(VfsCurrentPath, "/");
        VfsCurrentDrive[0] = '\0';
        return;
    }

    VfsCurrentDrive[0] = '\0';
    Sep = StrStr(VfsCurrentPath, "::");
    if (Sep) {
        Len = (USIZE)(Sep - VfsCurrentPath);
        if (Len < sizeof(VfsCurrentDrive)) {
            StrnCpy(VfsCurrentDrive, VfsCurrentPath, Len);
            VfsCurrentDrive[Len] = '\0';
        }
    } else {
        M = VfsMountForInode(VfsCurrentDir);
        if (M)
            StrCpy(VfsCurrentDrive, M->DriveName);
    }

    DisplayPath = VfsCurrentPath;
    Sep = StrStr(DisplayPath, "::");
    if (Sep)
        DisplayPath = Sep + 2;
    SnPrintf(Prompt, sizeof(Prompt), "%s> ", DisplayPath);
    BaseconSetPrompt(Prompt);
    BaseconRedrawFull();
}

INT VfsGetPathDepth(VfsInode *Inode) {
    INT Depth = 0;
    VfsInode *Current;
    VfsInode *Parent;

    if (!Inode)
        return 0;

    VfsInodeRef(Inode);
    Current = Inode;

    while (Current && !VfsIsNamespaceRoot(Current) && Depth < 1000) {
        if (VfsFindDriveMountByRoot(Current))
            break;
        if (VfsParent(Current, &Parent) != 0)
            break;
        if (Parent == Current) {
            VfsInodeUnref(Parent);
            break;
        }
        VfsInodeUnref(Current);
        Current = Parent;
        Depth++;
    }
    VfsInodeUnref(Current);
    return Depth;
}

INT BuildCurrentPath(VfsInode *Dir, const CHAR *Component, CHAR *Out) {
    CHAR BasePath[PATH_MAX];

    if (!Dir || !Out)
        return -1;
    if (VfsBuildPath(Dir, BasePath, PATH_MAX) != 0)
        return -1;
    if (!Component || Component[0] == '\0') {
        StrCpy(Out, BasePath);
        return 0;
    }
    if (BasePath[StrLen(BasePath) - 1] == '/')
        SnPrintf(Out, PATH_MAX, "%s%s", BasePath, Component);
    else
        SnPrintf(Out, PATH_MAX, "%s/%s", BasePath, Component);
    return 0;
}

VOID VfsInit(VOID) {
    GFsList = NULLPTR;
    GDriveMounts = NULLPTR;
    GNextIno = 1;

    VfsRootInode = VfsAllocInode();
    if (!VfsRootInode) {
        BaseconPrintf(BASECON_TYPE_ERROR, "vfs: failed to allocate root inode\n");
        return;
    }

    VfsRootInode->IMode = FT_DIR;
    VfsRootInode->IOp = &GNamespaceOps;
    VfsRootInode->ISize = 0;
    VfsRootInode->IDev = NULLPTR;
    StrCpy(VfsRootInode->IFsName, "vfs");

    VfsCurrentDir = VfsRootInode;
    VfsInodeRef(VfsCurrentDir);
    StrCpy(VfsCurrentPath, "/");
    VfsCurrentDrive[0] = '\0';

    BaseconPrintf(BASECON_TYPE_NORMAL, "vfs: initialized\n");
}
