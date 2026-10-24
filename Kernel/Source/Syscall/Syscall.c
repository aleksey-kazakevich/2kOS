#include <Drivers/Disk/DiskMgr.h>
#include <Utils/ExceptionLog.h>
#include <Utils/SyscallLog.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Drivers/Power.h>
#include <Drivers/Hpet.h>
#include <Linux/Fcntl.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Scheduler.h>
#include <Io/StdOut.h>
#include <Lib/StdIo.h>
#include <KObj/KObj.h>
#include <Syscall.h>
#include <Fs/Vfs.h>
#include <Return.h>
#include <Errno.h>
#include <Smp.h>

#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS

#define USER_MMAP_START   0x0000000010000000ULL
#define USER_MMAP_MAX     0x00007F0000000000ULL

/* Linux reboot(2) magic values — the kernel expects these exactly. */
#define LINUX_RB_AUTOBOOT      0x01234567
#define LINUX_RB_HALT_SYSTEM   0xCdef0123
#define LINUX_RB_POWER_OFF     0x4321fedc
#define LINUX_RB_KEXEC         0x45584543
#define LINUX_RB_SW_SUSPEND    0xd000fce2

/* Operations for SYS_2KOS_ELOG. */
#define ELOG_OP_LIST           0
#define ELOG_OP_LAST           1
#define ELOG_OP_CLEAR          2
#define ELOG_OP_COUNT          3

/* Operations for SYS_2KOS_SLOG. */
#define SLOG_OP_LIST           0
#define SLOG_OP_CLEAR          1
#define SLOG_OP_COUNT          2
#define SLOG_OP_ENABLE         3   /* arg1 = 1 = on, 0 = off */
#define SLOG_OP_IS_ENABLED     4

#define MNT_FORCE       0x00000001   /* force unmount */
#define MNT_DETACH      0x00000002   /* lazy unmount */
#define MNT_EXPIRE      0x00000004   /* mark for expiry */
#define UMOUNT_NOFOLLOW 0x00000008   /* do not follow symlink */
#define UMOUNT_FLAGS_MASK (MNT_FORCE | MNT_DETACH | MNT_EXPIRE | UMOUNT_NOFOLLOW)

#define MAP_FAILED   ((UINT64)-1)

/* struct linux_dirent64 — Linux layout. */
typedef struct {
    UINT64 d_ino;
    INT64  d_off;
    UINT16 d_reclen;
    UINT8  d_type;
    CHAR   d_name[];
} ATTRIBUTE(packed) LinuxDirent64;

/* struct stat — Linux x86_64 layout. 144 bytes. */
typedef struct {
    UINT64 st_dev;
    UINT64 st_ino;
    UINT64 st_nlink;
    UINT32 st_mode;
    UINT32 st_uid;
    UINT32 st_gid;
    UINT32 __pad0;
    UINT64 st_rdev;
    INT64  st_size;
    INT64  st_blksize;
    INT64  st_blocks;
    INT64  st_atime;
    INT64  st_atime_nsec;
    INT64  st_mtime;
    INT64  st_mtime_nsec;
    INT64  st_ctime;
    INT64  st_ctime_nsec;
    INT64  __unused[3];
} ATTRIBUTE(packed) LinuxStat64;

_Static_assert(sizeof(LinuxStat64) == 144,
               "LinuxStat64 must be exactly 144 bytes (Linux x86_64 ABI)");
_Static_assert(sizeof(LinuxDirent64) == 19,
               "LinuxDirent64 header must be 19 bytes (d_name is flexible)");

STATIC SyscallFunction GSyscallTable[SYSCALL_MAX] = {0};
STATIC UINT32 GSyscallCount = 0;

extern Timer GTimer;

STATIC INT LinuxToVfsOpenFlags(UINT32 LinuxFlags, UINT32 *OutVfsFlags) {
    UINT32 Access = LinuxFlags & O_ACCMODE;
    UINT32 Vfs = 0;

    switch (Access) {
    case O_RDONLY: Vfs = VFS_O_READ;                break;
    case O_WRONLY: Vfs = VFS_O_WRITE;               break;
    case O_RDWR:   Vfs = VFS_O_READ | VFS_O_WRITE;  break;
    default:       return -EINVAL;
    }

    if (LinuxFlags & O_CREAT)  Vfs |= VFS_O_CREAT;
    if (LinuxFlags & O_TRUNC)  Vfs |= VFS_O_TRUNC;
    if (LinuxFlags & O_APPEND) Vfs |= VFS_O_APPEND;
    if (LinuxFlags & O_EXCL)   Vfs |= VFS_O_EXCL;

    *OutVfsFlags = Vfs;
    return 0;
}

STATIC BOOL IsUserPointer(VOID *Ptr) {
    UINT64 Addr = (UINT64)Ptr;
    /* User addresses live below the kernel half and above the code base. */
    return (Addr >= USER_CODE_VADDR && Addr < 0x00007FFFFFFFFFFFULL);
}

STATIC INT CopyFromUser(VOID *Dest, const VOID *Src, USIZE Size) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return -EINVAL;

    UINT8 *D = (UINT8 *)Dest;
    UINT64 SrcAddr = (UINT64)Src;

    while (Size > 0) {
        UINT64 Phys = PagingLookupVirt(Current->PageTable, SrcAddr);
        if (!Phys) return -EFAULT;

        UINT64 Off = SrcAddr & (PAGE_SIZE - 1);
        USIZE Chunk = PAGE_SIZE - Off;
        if (Chunk > Size) Chunk = Size;

        MemCpy(D, (VOID *)PhysToVirt(Phys), Chunk);

        D += Chunk;
        SrcAddr += Chunk;
        Size -= Chunk;
    }
    return 0;
}

/* Resolve the base directory for *at() syscalls. */
STATIC VfsInode *SyscallResolveDirBase(INT DirFd) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return NULLPTR;

    if (DirFd == AT_FDCWD) {
        VfsInode *Base = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
        if (!Base) return NULLPTR;
        VfsInodeRef(Base);
        return Base;
    }

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)DirFd);
    if (!E || E->Kind != FD_KIND_VFS) return NULLPTR;
    if (E->File->FInode->IMode != FT_DIR) return NULLPTR;

    VfsInodeRef(E->File->FInode);
    return E->File->FInode;
}

/*
 * Copy a NUL-terminated string from user space with a hard length limit.
 * Returns 0 on success, -1 on failure (invalid pointer, no NUL, etc.).
 */
STATIC INT CopyStringFromUserBounded(const CHAR *UserStr, CHAR *KernelStr, USIZE MaxLen) {
    if (!UserStr || !KernelStr || MaxLen == 0) return -EINVAL;
    if (!IsUserPointer((VOID *)UserStr))       return -EFAULT;

    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->PageTable) return -EINVAL;

    for (USIZE I = 0; I < MaxLen - 1; I++) {
        UINT64 Addr = (UINT64)(UserStr + I);

        UINT64 Phys = PagingLookupVirt(Cur->PageTable, Addr);
        if (!Phys) return -EFAULT;

        CHAR C = *(volatile CHAR *)PhysToVirt(Phys);
        KernelStr[I] = C;
        if (C == '\0') return 0;
    }
    /* No NUL within MaxLen — reject (do not silently truncate). */
    KernelStr[MaxLen - 1] = '\0';
    return -ENAMETOOLONG;
}

INT CopyToUser(VOID *Dest, const VOID *Src, USIZE Size) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->PageTable) return -EINVAL;
    if (!IsUserPointer(Dest))             return -EFAULT;

    const UINT8 *S = (const UINT8 *)Src;
    UINT64 DstAddr = (UINT64)Dest;

    while (Size > 0) {
        UINT64 Phys = PagingLookupVirt(Current->PageTable, DstAddr);
        if (!Phys) return -EFAULT;

        UINT64 Off = DstAddr & (PAGE_SIZE - 1);
        USIZE Chunk = PAGE_SIZE - Off;
        if (Chunk > Size) Chunk = Size;

        MemCpy((VOID *)PhysToVirt(Phys), S, Chunk);

        S += Chunk;
        DstAddr += Chunk;
        Size -= Chunk;
    }
    return 0;
}

STATIC UINT64 SysWriteHandler(UINT64 Fd, UINT64 BufPtr, UINT64 Count) {
    if (Count == 0) return 0;
    if (Count > 65536) Count = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    Task *Cur = GetCurrentTask();
    if (!Cur) return SYSCALL_ERR(ESRCH);

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E) return SYSCALL_ERR(EBADF);

    UINT32 Access = E->Flags & O_ACCMODE;
    if (Access != O_WRONLY && Access != O_RDWR) return SYSCALL_ERR(EBADF);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Count);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    if (CopyFromUser(KernelBuf, (const VOID *)BufPtr, Count) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }

    UINT32 Written = 0;
    INT Rc;

    if (E->Kind == FD_KIND_VFS) {
        /* Honor O_APPEND: seek to end before writing. */
        if (E->Flags & O_APPEND) {
            VfsSeek(E->File, 0, SEEK_END);
        }
        Rc = VfsWrite(E->File, KernelBuf, (UINT32)Count, &Written);
        if (Rc == SUCCESS) E->Offset = E->File->FPos;
    } else if (E->Kind == FD_KIND_KOBJ) {
        if (!E->Obj->Ops || !E->Obj->Ops->Write) {
            MemoryFree(KernelBuf);
            return SYSCALL_ERR(EINVAL);
        }
        Rc = E->Obj->Ops->Write(E->Obj, 0, KernelBuf, (UINT32)Count, &Written);
    } else {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EBADF);
    }

    MemoryFree(KernelBuf);
    if (Rc != SUCCESS) return SYSCALL_ERR(EIO);
    return (UINT64)Written;
}

STATIC UINT64 SysExitHandler(UINT64 Code) {
    TaskExit((INT)Code);
    return 0;
}

STATIC UINT64 SysGetPidHandler(VOID) {
    Task *Current = GetCurrentTask();
    if (!Current) return SYSCALL_ERR(ESRCH);
    return (UINT64)Current->Pid;
}

STATIC UINT64 TimerGetMicroseconds(VOID) {
    if (HpetIsAvailable()) {
        UINT64 Freq = HpetGetFrequency();
        UINT64 Counter = HpetReadCounter();
        if (Freq == 0) return 0;
        return (Counter * 1000000ULL) / Freq;
    }
    // APIC: 1000 Hz, tick = 1 ms = 1000 us
    return GTimer.Ticks * 1000ULL;
}

STATIC UINT64 Sys2KosGetTicksHandler(VOID) {
    if (HpetIsAvailable()) {
        UINT64 Freq = HpetGetFrequency();
        UINT64 Counter = HpetReadCounter();
        if (Freq == 0) return 0;
        return (Counter * 1000000ULL) / Freq;
    }
    return GTimer.Ticks * 1000ULL;
}

STATIC UINT64 SysBrkHandler(UINT64 NewBrk) {
    Task *Current = GetCurrentTask();
    if (!Current || !Current->IsUser) return SYSCALL_ERR(ESRCH);

    UINT64 OldBrk = Current->BrkCurrent;

    /* brk(0) — query the current break, do not change it. */
    if (NewBrk == 0) return OldBrk;

    if (NewBrk < Current->BrkStart) return OldBrk;   /* Linux semantics: return old */
    if (NewBrk > Current->BrkMax)   return OldBrk;

    UINT64 OldPage = (OldBrk  + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    UINT64 NewPage = (NewBrk + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);

    if (NewPage > OldPage) {
        UINT64 Flags = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NO_EXEC;

        UINT64 FirstMapped = 0;
        BOOL   AnyMapped   = FALSE;

        for (UINT64 Va = OldPage; Va < NewPage; Va += PAGE_SIZE) {
            if (PagingLookupVirt(Current->PageTable, Va) != 0) continue;

            VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
            if (!Phys) {
                if (AnyMapped) {
                    for (UINT64 R = FirstMapped; R < Va; R += PAGE_SIZE) {
                        UINT64 RP = PagingLookupVirt(Current->PageTable, R);
                        if (RP) {
                            PagingUnmapPage(Current->PageTable, R);
                            PhysAllocFreePage(PhysAllocGet(),
                                (VOID *)(RP & ~(UINT64)(PAGE_SIZE - 1)));
                        }
                    }
                }
                /* Linux returns the old break on failure. */
                return OldBrk;
            }

            MemSet((VOID *)PhysToVirt((UINT64)Phys), 0, PAGE_SIZE);

            if (PagingMapPage(Current->PageTable, Va, (UINT64)Phys, Flags) != SUCCESS) {
                PhysAllocFreePage(PhysAllocGet(), Phys);
                if (AnyMapped) {
                    for (UINT64 R = FirstMapped; R < Va; R += PAGE_SIZE) {
                        UINT64 RP = PagingLookupVirt(Current->PageTable, R);
                        if (RP) {
                            PagingUnmapPage(Current->PageTable, R);
                            PhysAllocFreePage(PhysAllocGet(),
                                (VOID *)(RP & ~(UINT64)(PAGE_SIZE - 1)));
                        }
                    }
                }
                return OldBrk;
            }

            if (!AnyMapped) { FirstMapped = Va; AnyMapped = TRUE; }
        }
    } else if (NewPage < OldPage) {
        for (UINT64 Va = NewPage; Va < OldPage; Va += PAGE_SIZE) {
            UINT64 Phys = PagingLookupVirt(Current->PageTable, Va);
            if (Phys) {
                PagingUnmapPage(Current->PageTable, Va);
                PhysAllocFreePage(PhysAllocGet(),
                                  (VOID *)(Phys & ~(UINT64)(PAGE_SIZE - 1)));
            }
        }
    }

    Current->BrkCurrent = NewBrk;
    return NewBrk;
}

STATIC UINT64 SysMmapHandler(UINT64 Addr, UINT64 Len, UINT64 Prot,
                              UINT64 Flags, UINT64 Fd, UINT64 Off) {
    (VOID)Fd;
    (VOID)Off;

    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser || !Cur->PageTable) return SYSCALL_ERR(ESRCH);

    if (Len == 0) return SYSCALL_ERR(EINVAL);

    Len = (Len + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    if (Len == 0) return SYSCALL_ERR(EINVAL);   /* overflow */

    BOOL IsFixed = (Flags & MAP_FIXED) != 0;
    BOOL IsAnon  = (Flags & MAP_ANONYMOUS) != 0;
    if (!IsAnon) return SYSCALL_ERR(ENOSYS);    /* file-backed not implemented */

    UINT64 PteFlags = PTE_PRESENT | PTE_USER;
    if (Prot & PROT_WRITE) PteFlags |= PTE_WRITABLE;
    if (!(Prot & PROT_EXEC)) PteFlags |= PTE_NO_EXEC;

    UINT64 Target = Addr;

    if (!IsFixed) {
        Target = USER_MMAP_START;

        for (UINT64 Va = USER_MMAP_START; Va + Len <= USER_MMAP_MAX; Va += PAGE_SIZE) {
            BOOL Free = TRUE;
            for (UINT64 Check = Va; Check < Va + Len; Check += PAGE_SIZE) {
                if (PagingLookupVirt(Cur->PageTable, Check) != 0) {
                    Free = FALSE;
                    break;
                }
            }
            if (Free) { Target = Va; break; }
            Target = 0;
        }
        if (Target == 0) return SYSCALL_ERR(ENOMEM);
    } else {
        if (Addr == 0) return SYSCALL_ERR(EINVAL);
        if ((Addr & (PAGE_SIZE - 1)) != 0) return SYSCALL_ERR(EINVAL);
        if (Addr + Len < Addr) return SYSCALL_ERR(EINVAL);   /* overflow */

        for (UINT64 Va = Addr; Va < Addr + Len; Va += PAGE_SIZE) {
            UINT64 Phys = PagingLookupVirt(Cur->PageTable, Va);
            if (Phys) {
                PagingUnmapPage(Cur->PageTable, Va);
                PhysAllocFreePage(PhysAllocGet(),
                                  (VOID *)(Phys & ~(UINT64)(PAGE_SIZE - 1)));
            }
        }
    }

    for (UINT64 Va = Target; Va < Target + Len; Va += PAGE_SIZE) {
        VOID *Phys = PhysAllocAllocatePage(PhysAllocGet());
        if (!Phys) {
            for (UINT64 Back = Target; Back < Va; Back += PAGE_SIZE) {
                UINT64 P = PagingLookupVirt(Cur->PageTable, Back);
                if (P) {
                    PagingUnmapPage(Cur->PageTable, Back);
                    PhysAllocFreePage(PhysAllocGet(),
                                      (VOID *)(P & ~(UINT64)(PAGE_SIZE - 1)));
                }
            }
            return SYSCALL_ERR(ENOMEM);
        }
        MemSet((VOID *)PhysToVirt((UINT64)Phys), 0, PAGE_SIZE);
        if (PagingMapPage(Cur->PageTable, Va, (UINT64)Phys, PteFlags) != SUCCESS) {
            PhysAllocFreePage(PhysAllocGet(), Phys);
            for (UINT64 Back = Target; Back < Va; Back += PAGE_SIZE) {
                UINT64 P = PagingLookupVirt(Cur->PageTable, Back);
                if (P) {
                    PagingUnmapPage(Cur->PageTable, Back);
                    PhysAllocFreePage(PhysAllocGet(),
                                      (VOID *)(P & ~(UINT64)(PAGE_SIZE - 1)));
                }
            }
            return SYSCALL_ERR(ENOMEM);
        }
    }

    return Target;
}

STATIC UINT64 SysMunmapHandler(UINT64 Addr, UINT64 Len) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser || !Cur->PageTable) return SYSCALL_ERR(ESRCH);

    if (Len == 0) return SYSCALL_ERR(EINVAL);
    if ((Addr & (PAGE_SIZE - 1)) != 0) return SYSCALL_ERR(EINVAL);

    Len = (Len + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    if (Len == 0) return SYSCALL_ERR(EINVAL);
    if (Addr + Len < Addr) return SYSCALL_ERR(EINVAL);

    if (Addr < USER_CODE_VADDR) return SYSCALL_ERR(EINVAL);
    if (Addr + Len > USER_STACK_TOP - USER_STACK_DEFAULT) return SYSCALL_ERR(EINVAL);

    for (UINT64 Va = Addr; Va < Addr + Len; Va += PAGE_SIZE) {
        UINT64 Phys = PagingLookupVirt(Cur->PageTable, Va);
        if (Phys) {
            PagingUnmapPage(Cur->PageTable, Va);
            PhysAllocFreePage(PhysAllocGet(),
                              (VOID *)(Phys & ~(UINT64)(PAGE_SIZE - 1)));
        }
    }
    return 0;
}

STATIC UINT64 SysWriteChannelCommon(StdIoChannel Ch, UINT64 BufPtr, UINT64 Size) {
    if (Size == 0) return 0;
    if (Size > 65536) Size = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Size);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    if (CopyFromUser(KernelBuf, (const VOID *)BufPtr, Size) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }

    INT W = StdIoWriteChannel(Ch, KernelBuf, (UINT32)Size);
    MemoryFree(KernelBuf);
    if (W < 0) return SYSCALL_ERR(EIO);
    return (UINT64)W;
}

STATIC UINT64 Sys2KosStdoutWriteHandler(UINT64 BufPtr, UINT64 Size) {
    return SysWriteChannelCommon(STDIO_CHAN_STDOUT, BufPtr, Size);
}
STATIC UINT64 Sys2KosStderrWriteHandler(UINT64 BufPtr, UINT64 Size) {
    return SysWriteChannelCommon(STDIO_CHAN_STDERR, BufPtr, Size);
}
STATIC UINT64 Sys2KosStdwarnWriteHandler(UINT64 BufPtr, UINT64 Size) {
    return SysWriteChannelCommon(STDIO_CHAN_STDWARN, BufPtr, Size);
}

STATIC UINT64 Sys2KosStdinReadHandler(UINT64 BufPtr, UINT64 Size) {
    if (Size == 0) return 0;
    if (Size > 65536) Size = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Size);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    UINT32 Got = 0;
    INT Rc = StdIoReadChannel(STDIO_CHAN_STDIN, KernelBuf, (UINT32)Size, &Got);
    if (Rc != SUCCESS) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EIO);
    }
    if (Got > 0 && CopyToUser((VOID *)BufPtr, KernelBuf, Got) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }
    MemoryFree(KernelBuf);
    return (UINT64)Got;
}

STATIC UINT64 Sys2KosKObjOpenHandler(UINT64 PathPtr, UINT64 Access) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(ESRCH);

    if (Access == 0 || (Access & ~3ULL) != 0) return SYSCALL_ERR(EINVAL);

    CHAR Path[KOBJ_PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));   /* Rc is already negative */

    KObj *Obj = KObjFindByPath(Path);
    if (!Obj) return SYSCALL_ERR(ENOENT);

    if ((Obj->Access & (UINT32)Access) != (UINT32)Access) {
        KObjUnref(Obj);
        return SYSCALL_ERR(EACCES);
    }

    INT H = KObjHandleInstall(Obj, (UINT32)Access);
    if (H < 0) {
        KObjUnref(Obj);
        return SYSCALL_ERR(EMFILE);
    }

    return (UINT64)H;
}

STATIC UINT64 Sys2KosKObjCloseHandler(UINT64 Handle) {
    if (Handle >= KOBJ_HANDLE_MAX) return SYSCALL_ERR(EBADF);
    return KObjHandleClose((UINT32)Handle) == 0 ? 0 : SYSCALL_ERR(EBADF);
}

STATIC UINT64 Sys2KosKObjReadHandler(UINT64 Handle, UINT64 BufPtr, UINT64 Size) {
    if (Handle >= KOBJ_HANDLE_MAX) return SYSCALL_ERR(EBADF);
    if (Size == 0) return 0;
    if (Size > 65536) Size = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    KObj *Obj = KObjHandleGet((UINT32)Handle, KOBJ_ACCESS_READ);
    if (!Obj) return SYSCALL_ERR(EBADF);
    if (!Obj->Ops || !Obj->Ops->Read) return SYSCALL_ERR(EINVAL);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Size);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    UINT32 Got = 0;
    INT Rc = Obj->Ops->Read(Obj, 0, KernelBuf, (UINT32)Size, &Got);
    if (Rc != SUCCESS) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EIO);
    }

    if (Got > 0 && CopyToUser((VOID *)BufPtr, KernelBuf, Got) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }
    MemoryFree(KernelBuf);
    return (UINT64)Got;
}

STATIC UINT64 Sys2KosKObjWriteHandler(UINT64 Handle, UINT64 BufPtr, UINT64 Size) {
    if (Handle >= KOBJ_HANDLE_MAX) return SYSCALL_ERR(EBADF);
    if (Size == 0) return 0;
    if (Size > 65536) Size = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    KObj *Obj = KObjHandleGet((UINT32)Handle, KOBJ_ACCESS_WRITE);
    if (!Obj) return SYSCALL_ERR(EBADF);
    if (!Obj->Ops || !Obj->Ops->Write) return SYSCALL_ERR(EINVAL);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Size);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    if (CopyFromUser(KernelBuf, (const VOID *)BufPtr, Size) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }

    UINT32 Written = 0;
    INT Rc = Obj->Ops->Write(Obj, 0, KernelBuf, (UINT32)Size, &Written);
    MemoryFree(KernelBuf);
    if (Rc != SUCCESS) return SYSCALL_ERR(EIO);
    return (UINT64)Written;
}

STATIC UINT64 SysReadHandler(UINT64 Fd, UINT64 BufPtr, UINT64 Count) {
    if (Count == 0) return 0;
    if (Count > 65536) Count = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    Task *Cur = GetCurrentTask();
    if (!Cur) return SYSCALL_ERR(ESRCH);

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E) return SYSCALL_ERR(EBADF);
    UINT32 Access = E->Flags & O_ACCMODE;
    if (Access != O_RDONLY && Access != O_RDWR) return SYSCALL_ERR(EBADF);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Count);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    UINT32 Got = 0;
    INT Rc;

    if (E->Kind == FD_KIND_VFS) {
        Rc = VfsRead(E->File, KernelBuf, (UINT32)Count, &Got);
        if (Rc == SUCCESS) E->Offset = E->File->FPos;
    } else if (E->Kind == FD_KIND_KOBJ) {
        if (!E->Obj->Ops || !E->Obj->Ops->Read) {
            MemoryFree(KernelBuf);
            return SYSCALL_ERR(EINVAL);
        }
        Rc = E->Obj->Ops->Read(E->Obj, 0, KernelBuf, (UINT32)Count, &Got);
    } else {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EBADF);
    }

    if (Rc != SUCCESS) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EIO);
    }

    if (Got > 0 && CopyToUser((VOID *)BufPtr, KernelBuf, Got) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }

    MemoryFree(KernelBuf);
    return (UINT64)Got;
}

STATIC UINT64 SysOpenAtHandler(UINT64 DirFd, UINT64 PathPtr,
                               UINT64 Flags, UINT64 Mode) {
    (VOID)Mode;

    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(ESRCH);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *BaseDir;
    if ((INT)DirFd == AT_FDCWD) {
        BaseDir = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
        if (!BaseDir) return SYSCALL_ERR(ENOENT);
        VfsInodeRef(BaseDir);
    } else {
        FdEntry *DirE = FdGet(&Cur->Fds, (UINT32)DirFd);
        if (!DirE || DirE->Kind != FD_KIND_VFS) return SYSCALL_ERR(EBADF);
        if (DirE->File->FInode->IMode != FT_DIR) return SYSCALL_ERR(ENOTDIR);
        BaseDir = DirE->File->FInode;
        VfsInodeRef(BaseDir);
    }

    UINT32 VfsFlags = 0;
    Rc = LinuxToVfsOpenFlags((UINT32)Flags, &VfsFlags);
    if (Rc != 0) {
        VfsInodeUnref(BaseDir);
        return SYSCALL_ERR(EINVAL);
    }

    VfsFile *File = NULLPTR;
    Rc = VfsOpen(BaseDir, Path, VfsFlags, &File);
    VfsInodeUnref(BaseDir);

    if (Rc != SUCCESS) {
        switch (Rc) {
            case NOT_FOUND:         return SYSCALL_ERR(ENOENT);
            case NO_MEMORY:         return SYSCALL_ERR(ENOMEM);
            case ALREADY_EXISTS:    return SYSCALL_ERR(EEXIST);
            case PERMISSION_DENIED: return SYSCALL_ERR(EACCES);
            case NOT_SUPPORTED:     return SYSCALL_ERR(ENOSYS);
            default:                return SYSCALL_ERR(EIO);
        }
    }

    if ((Flags & O_DIRECTORY) && File->FInode->IMode != FT_DIR) {
        VfsClose(File);
        return SYSCALL_ERR(ENOTDIR);
    }

    INT Fd = FdInstallVfs(&Cur->Fds, File, (UINT32)Flags);
    if (Fd < 0) {
        VfsClose(File);
        return SYSCALL_ERR(EMFILE);
    }
    return (UINT64)Fd;
}

STATIC UINT64 SysOpenHandler(UINT64 PathPtr, UINT64 Flags, UINT64 Mode) {
    return SysOpenAtHandler((UINT64)(INT64)AT_FDCWD, PathPtr, Flags, Mode);
}

STATIC UINT64 SysCloseHandler(UINT64 Fd) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return SYSCALL_ERR(ESRCH);

    if (Fd >= FD_MAX) return SYSCALL_ERR(EBADF);
    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E) return SYSCALL_ERR(EBADF);

    FdFree(&Cur->Fds, (UINT32)Fd);
    return 0;
}

STATIC UINT64 SysLseekHandler(UINT64 Fd, UINT64 Offset, UINT64 Whence) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return SYSCALL_ERR(ESRCH);

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E) return SYSCALL_ERR(EBADF);
    if (E->Kind != FD_KIND_VFS) return SYSCALL_ERR(ESPIPE);   /* pipes/sockets */

    INT Rc = VfsSeek(E->File, Offset, (INT)Whence);
    if (Rc != SUCCESS) return SYSCALL_ERR(EINVAL);

    E->Offset = E->File->FPos;
    return E->Offset;
}

STATIC VOID VfsStatToLinux(VfsStats *In, LinuxStat64 *Out) {
    MemSet(Out, 0, sizeof(LinuxStat64));
    Out->st_dev     = 0;
    Out->st_ino     = In->StIno;
    Out->st_nlink   = In->StNlink;
    Out->st_mode    = In->StMode;
    Out->st_uid     = In->StUid;
    Out->st_gid     = In->StGid;
    Out->st_rdev    = 0;
    Out->st_size    = (INT64)In->StSize;
    Out->st_blksize = 4096;
    Out->st_blocks  = (INT64)((In->StSize + 511) / 512);
    Out->st_atime   = (INT64)In->StAtime;
    Out->st_mtime   = (INT64)In->StMtime;
    Out->st_ctime   = (INT64)In->StCtime;
}

STATIC UINT64 SysFstatHandler(UINT64 Fd, UINT64 StatBufPtr) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return SYSCALL_ERR(ESRCH);
    if (!IsUserPointer((VOID *)StatBufPtr)) return SYSCALL_ERR(EFAULT);

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E) return SYSCALL_ERR(EBADF);
    if (E->Kind != FD_KIND_VFS) return SYSCALL_ERR(EINVAL);

    VfsStats St;
    INT Rc = VfsStat(E->File->FInode, &St);
    if (Rc != SUCCESS) return SYSCALL_ERR(EIO);

    LinuxStat64 LSt;
    VfsStatToLinux(&St, &LSt);

    if (CopyToUser((VOID *)StatBufPtr, &LSt, sizeof(LSt)) != 0)
        return SYSCALL_ERR(EFAULT);
    return 0;
}

STATIC UINT64 SysNewFstatAtHandler(UINT64 DirFd, UINT64 PathPtr,
                                    UINT64 StatBufPtr, UINT64 Flags) {
    (VOID)Flags;
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(ESRCH);
    if (!IsUserPointer((VOID *)StatBufPtr)) return SYSCALL_ERR(EFAULT);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *BaseDir = NULLPTR;
    if ((INT)DirFd == AT_FDCWD) {
        BaseDir = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
        if (!BaseDir) return SYSCALL_ERR(ENOENT);
        VfsInodeRef(BaseDir);
    } else {
        FdEntry *DirE = FdGet(&Cur->Fds, (UINT32)DirFd);
        if (!DirE || DirE->Kind != FD_KIND_VFS) return SYSCALL_ERR(EBADF);
        BaseDir = DirE->File->FInode;
        VfsInodeRef(BaseDir);
    }

    VfsInode *Inode = NULLPTR;
    Rc = VfsWalk(BaseDir, Path, &Inode);
    if (Rc != SUCCESS) return SYSCALL_ERR(ENOENT);

    VfsStats St;
    VfsStat(Inode, &St);
    VfsInodeUnref(Inode);

    LinuxStat64 LSt;
    VfsStatToLinux(&St, &LSt);

    if (CopyToUser((VOID *)StatBufPtr, &LSt, sizeof(LSt)) != 0)
        return SYSCALL_ERR(EFAULT);
    return 0;
}

STATIC UINT64 SysStatHandler(UINT64 PathPtr, UINT64 StatBufPtr, UINT64 Unused) {
    (VOID)Unused;
    return SysNewFstatAtHandler((UINT64)(INT64)AT_FDCWD, PathPtr, StatBufPtr, 0);
}

INT SyscallRegister(UINT64 Num, SyscallFunction Func) {
    if (Num >= SYSCALL_MAX) return INCORRECT_VALUE;
    if (GSyscallTable[Num] != NULLPTR) return ALREADY_EXISTS;
    
    GSyscallTable[Num] = Func;
    GSyscallCount++;
    return SUCCESS;
}

STATIC UINT64 SysGetdents64Handler(UINT64 Fd, UINT64 DirpPtr, UINT64 Count) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return SYSCALL_ERR(ESRCH);
    if (Count == 0) return SYSCALL_ERR(EINVAL);
    if (Count > 65536) Count = 65536;
    if (!IsUserPointer((VOID *)DirpPtr)) return SYSCALL_ERR(EFAULT);

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E || E->Kind != FD_KIND_VFS) return SYSCALL_ERR(EBADF);

    VfsInode *Dir = E->File->FInode;
    if (Dir->IMode != FT_DIR) return SYSCALL_ERR(ENOTDIR);

    UINT8 *KernelBuf = (UINT8 *)MemoryAllocate(Count);
    if (!KernelBuf) return SYSCALL_ERR(ENOMEM);

    UINT64 Pos = E->Offset;
    UINT64 Used = 0;

    while (Used + 32 <= Count) {
        CHAR Name[NAME_MAX + 1];
        UINT32 NameLen = 0;
        UINT32 Type = 0;
        UINT64 SavePos = Pos;

        INT Rc = VfsReadDir(Dir, &Pos, Name, &NameLen, &Type);
        if (Rc != SUCCESS) break;

        if ((NameLen == 1 && Name[0] == '.') ||
            (NameLen == 2 && Name[0] == '.' && Name[1] == '.')) {
            continue;
        }

        UINT16 Reclen = (UINT16)(19 + NameLen + 1);
        Reclen = (Reclen + 7) & ~7;

        if (Used + Reclen > Count) {
            Pos = SavePos;
            break;
        }

        LinuxDirent64 *D = (LinuxDirent64 *)(KernelBuf + Used);
        D->d_ino    = 1;
        D->d_off    = (INT64)(Pos);
        D->d_reclen = Reclen;
        D->d_type   = (Type == FT_DIR)  ? 4 :
                      (Type == FT_REG_FILE) ? 8 : 0;
        MemCpy(D->d_name, Name, NameLen);
        D->d_name[NameLen] = '\0';

        Used += Reclen;
    }

    E->Offset = Pos;

    if (Used == 0) {
        MemoryFree(KernelBuf);
        return 0;
    }

    if (CopyToUser((VOID *)DirpPtr, KernelBuf, Used) != 0) {
        MemoryFree(KernelBuf);
        return SYSCALL_ERR(EFAULT);
    }

    MemoryFree(KernelBuf);
    return Used;
}

STATIC UINT64 SysChdirHandler(UINT64 PathPtr) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(ESRCH);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *BaseDir = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
    if (!BaseDir) return SYSCALL_ERR(ENOENT);

    VfsInode *NewDir = NULLPTR;
    Rc = VfsWalk(BaseDir, Path, &NewDir);
    if (Rc != SUCCESS) {
        switch (Rc) {
            case NOT_FOUND: return SYSCALL_ERR(ENOENT);
            case NO_MEMORY: return SYSCALL_ERR(ENOMEM);
            default:        return SYSCALL_ERR(ENOTDIR);
        }
    }

    if (NewDir->IMode != FT_DIR) {
        VfsInodeUnref(NewDir);
        return SYSCALL_ERR(ENOTDIR);
    }

    /* Replace the task's cwd. */
    VfsInode *Old = Cur->Cwd;
    Cur->Cwd = NewDir;
    /* NewDir already has refcount 1 from VfsWalk. */

    if (Old) VfsInodeUnref(Old);

    /* Keep the global VfsCurrentDir in sync so that the shell prompt
     * (Basecon) reflects the change. This is a compatibility shim;
     * a real multi-process system would drop VfsCurrentDir entirely. */
    if (VfsCurrentDir) VfsInodeUnref(VfsCurrentDir);
    VfsCurrentDir = NewDir;
    VfsInodeRef(VfsCurrentDir);

    VfsUpdateCurrentPath();
    return 0;
}

STATIC UINT64 SysGetcwdHandler(UINT64 BufPtr, UINT64 Size) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(ESRCH);
    if (Size == 0) return SYSCALL_ERR(EINVAL);
    if (Size > 65536) Size = 65536;
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    VfsInode *Dir = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
    if (!Dir) return SYSCALL_ERR(ENOENT);

    CHAR Path[PATH_MAX];
    if (VfsBuildPath(Dir, Path, sizeof(Path)) != 0) {
        return SYSCALL_ERR(ENOENT);
    }

    USIZE Len = StrLen(Path) + 1;   /* include NUL */
    if (Len > Size) return SYSCALL_ERR(ERANGE);

    if (CopyToUser((VOID *)BufPtr, Path, Len) != 0) {
        return SYSCALL_ERR(EFAULT);
    }
    return (UINT64)Len;
}

STATIC UINT64 SysFchdirHandler(UINT64 Fd) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(ESRCH);

    FdEntry *E = FdGet(&Cur->Fds, (UINT32)Fd);
    if (!E || E->Kind != FD_KIND_VFS) return SYSCALL_ERR(EBADF);
    if (E->File->FInode->IMode != FT_DIR) return SYSCALL_ERR(ENOTDIR);

    VfsInode *NewDir = E->File->FInode;
    VfsInodeRef(NewDir);

    VfsInode *Old = Cur->Cwd;
    Cur->Cwd = NewDir;
    if (Old) VfsInodeUnref(Old);

    if (VfsCurrentDir) VfsInodeUnref(VfsCurrentDir);
    VfsCurrentDir = NewDir;
    VfsInodeRef(VfsCurrentDir);

    VfsUpdateCurrentPath();
    return 0;
}

STATIC UINT64 SysRebootHandler(UINT64 Magic1, UINT64 Magic2, UINT64 Cmd,
                                UINT64 Arg, UINT64 Unused1, UINT64 Unused2) {
    (VOID)Arg; (VOID)Unused1; (VOID)Unused2;

    BOOL MagicOk = (Magic1 == 0x28121969 && Magic2 == 0x05121996) ||
                   (Magic1 == 0x05121996 && Magic2 == 0x16041998) ||
                   (Magic1 == 0x05121996 && Magic2 == 0x20112000) ||
                   (Magic1 == 0 && Magic2 == 0);   /* 2kOS legacy */
    if (!MagicOk) return SYSCALL_ERR(EINVAL);

    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);
    /* TODO: CAP_SYS_BOOT when capabilities exist. */

    switch ((UINT32)Cmd) {
        case LINUX_RB_AUTOBOOT:
            PowerReboot();
            return 0;

        case LINUX_RB_POWER_OFF:
            PowerShutdown();
            return 0;

        case LINUX_RB_HALT_SYSTEM:
            PowerHalt();
            return 0;

        case LINUX_RB_KEXEC:
        case LINUX_RB_SW_SUSPEND:
        default:
            return SYSCALL_ERR(ENOSYS);
    }
}

STATIC UINT64 SysMkdirHandler(UINT64 PathPtr, UINT64 Mode) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *Base = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
    if (!Base) return SYSCALL_ERR(ENOENT);

    VfsInode *NewDir = NULLPTR;
    Rc = VfsMkdir(Base, Path, (UINT32)Mode, &NewDir);
    if (Rc != SUCCESS) {
        switch (Rc) {
            case ALREADY_EXISTS: return SYSCALL_ERR(EEXIST);
            case NO_MEMORY:      return SYSCALL_ERR(ENOMEM);
            case NOT_FOUND:      return SYSCALL_ERR(ENOENT);
            case INCORRECT_VALUE: return SYSCALL_ERR(ENOTDIR);
            default:             return SYSCALL_ERR(EIO);
        }
    }
    VfsInodeUnref(NewDir);
    return 0;
}

STATIC UINT64 SysMkdirAtHandler(UINT64 DirFd, UINT64 PathPtr, UINT64 Mode) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *Base = SyscallResolveDirBase((INT)DirFd);
    if (!Base) return SYSCALL_ERR(EBADF);

    VfsInode *NewDir = NULLPTR;
    Rc = VfsMkdir(Base, Path, (UINT32)Mode, &NewDir);
    VfsInodeUnref(Base);

    if (Rc != SUCCESS) {
        switch (Rc) {
            case ALREADY_EXISTS: return SYSCALL_ERR(EEXIST);
            case NO_MEMORY:      return SYSCALL_ERR(ENOMEM);
            case NOT_FOUND:      return SYSCALL_ERR(ENOENT);
            case INCORRECT_VALUE: return SYSCALL_ERR(ENOTDIR);
            default:             return SYSCALL_ERR(EIO);
        }
    }
    VfsInodeUnref(NewDir);
    return 0;
}

STATIC UINT64 SysRmdirHandler(UINT64 PathPtr) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *Base = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
    if (!Base) return SYSCALL_ERR(ENOENT);

    Rc = VfsRmdir(Base, Path);
    if (Rc != SUCCESS) {
        switch (Rc) {
            case NOT_FOUND:       return SYSCALL_ERR(ENOENT);
            case INCORRECT_VALUE: return SYSCALL_ERR(ENOTEMPTY);
            default:              return SYSCALL_ERR(EIO);
        }
    }
    return 0;
}

STATIC UINT64 SysUnlinkHandler(UINT64 PathPtr) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *Base = Cur->Cwd ? Cur->Cwd : VfsCurrentDir;
    if (!Base) return SYSCALL_ERR(ENOENT);

    Rc = VfsUnlink(Base, Path);
    if (Rc != SUCCESS) {
        switch (Rc) {
            case NOT_FOUND:       return SYSCALL_ERR(ENOENT);
            case INCORRECT_VALUE: return SYSCALL_ERR(EISDIR);
            default:              return SYSCALL_ERR(EIO);
        }
    }
    return 0;
}

STATIC UINT64 SysUnlinkAtHandler(UINT64 DirFd, UINT64 PathPtr, UINT64 Flags) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);

    CHAR Path[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)PathPtr, Path, sizeof(Path));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    VfsInode *Base = SyscallResolveDirBase((INT)DirFd);
    if (!Base) return SYSCALL_ERR(EBADF);

    if (Flags & AT_REMOVEDIR) {
        Rc = VfsRmdir(Base, Path);
    } else {
        Rc = VfsUnlink(Base, Path);
    }
    VfsInodeUnref(Base);

    if (Rc != SUCCESS) {
        switch (Rc) {
            case NOT_FOUND:       return SYSCALL_ERR(ENOENT);
            case INCORRECT_VALUE: return SYSCALL_ERR(Flags & AT_REMOVEDIR ? ENOTEMPTY : EISDIR);
            default:              return SYSCALL_ERR(EIO);
        }
    }
    return 0;
}

STATIC UINT64 Sys2KosElogHandler(UINT64 Op, UINT64 Arg1, UINT64 Arg2) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);
    if (!Cur->PageTable) return SYSCALL_ERR(EFAULT);

    switch ((UINT32)Op) {
        case ELOG_OP_COUNT:
            return (UINT64)ExceptionLogCount();

        case ELOG_OP_CLEAR:
            ExceptionLogClear();
            return 0;

        case ELOG_OP_LIST: {
            UINT32 MaxEntries = (UINT32)Arg2;
            if (MaxEntries == 0) MaxEntries = EXCEPTION_LOG_SIZE;
            INT Rc = ExceptionLogCopyToUser((ExceptionLogEntry *)Arg1,
                                            MaxEntries);
            if (Rc < 0) return SYSCALL_ERR((UINT64)(-Rc));
            return (UINT64)Rc;
        }

        case ELOG_OP_LAST: {
            UINT32 Count = ExceptionLogCount();
            if (Count == 0) return SYSCALL_ERR(ENOENT);
            const ExceptionLogEntry *Last = ExceptionLogGet(Count - 1);
            if (!Last) return SYSCALL_ERR(ENOENT);
            INT Rc = PagingCopyToUser(Cur->PageTable,
                                      Arg1, Last,
                                      sizeof(ExceptionLogEntry));
            if (Rc != SUCCESS) return SYSCALL_ERR(EFAULT);
            return sizeof(ExceptionLogEntry);
        }

        default:
            return SYSCALL_ERR(EINVAL);
    }
}

STATIC UINT64 Sys2KosSlogHandler(UINT64 Op, UINT64 Arg1, UINT64 Arg2) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);
    if (!Cur->PageTable) return SYSCALL_ERR(EFAULT);

    switch ((UINT32)Op) {
        case SLOG_OP_COUNT:
            return (UINT64)SyscallLogCount();

        case SLOG_OP_CLEAR:
            SyscallLogClear();
            return 0;

        case SLOG_OP_ENABLE:
            SyscallLogSetEnabled(Arg1 != 0);
            return 0;

        case SLOG_OP_IS_ENABLED:
            return SyscallLogIsEnabled() ? 1 : 0;

        case SLOG_OP_LIST: {
            UINT32 MaxEntries = (UINT32)Arg2;
            if (MaxEntries == 0) MaxEntries = SYSCALL_LOG_SIZE;
            INT Rc = SyscallLogCopyToUser((SyscallLogEntry *)Arg1,
                                          MaxEntries);
            if (Rc < 0) return SYSCALL_ERR((UINT64)(-Rc));
            return (UINT64)Rc;
        }

        default:
            return SYSCALL_ERR(EINVAL);
    }
}

STATIC UINT64 SysUmount2Handler(UINT64 TargetPtr, UINT64 Flags) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);

    if (Flags & ~UMOUNT_FLAGS_MASK) return SYSCALL_ERR(EINVAL);

    CHAR Target[PATH_MAX];
    INT Rc = CopyStringFromUserBounded((const CHAR *)TargetPtr,
                                       Target, sizeof(Target));
    if (Rc != 0) return SYSCALL_ERR((UINT64)(-Rc));

    /* The kernel mount table is keyed by drive name, not by path.
     * Accept "<Drive>", "<Drive>::/", or "<Drive>::/path". */
    CHAR DriveName[DRIVE_NAME_MAX];
    UINT32 Len = 0;

    while (Target[Len] && Target[Len] != ':' && Len < DRIVE_NAME_MAX - 1) {
        DriveName[Len] = Target[Len];
        Len++;
    }
    DriveName[Len] = '\0';

    if (Len == 0) return SYSCALL_ERR(EINVAL);

    /* Make sure the drive is actually mounted. */
    CHAR Probe[DRIVE_NAME_MAX + 4];
    SnPrintf(Probe, sizeof(Probe), "%s::/", DriveName);
    VfsInode *Tmp = NULLPTR;
    if (VfsWalk(VfsRootInode, Probe, &Tmp) != SUCCESS)
        return SYSCALL_ERR(EINVAL);
    VfsInodeUnref(Tmp);

    /*
     * If the caller is inside the drive being unmounted, move them
     * to the namespace root first. VfsUnmountDrive would do this
     * anyway, but doing it here lets us return EBUSY if the move
     * fails instead of silently dropping the caller.
     */
    if (Cur->Cwd) {
        CHAR CwdPath[PATH_MAX];
        if (VfsBuildPath(Cur->Cwd, CwdPath, sizeof(CwdPath)) == SUCCESS) {
            if (StrStr(CwdPath, DriveName) != NULLPTR) {
                VfsInode *Root = VfsRootInode;
                VfsInodeRef(Root);
                VfsInodeUnref(Cur->Cwd);
                Cur->Cwd = Root;
            }
        }
    }

    INT Result = VfsUnmountDrive(DriveName);
    switch (Result) {
        case SUCCESS:    return 0;
        case NOT_FOUND:  return SYSCALL_ERR(ENOENT);
        case BUSY:       return SYSCALL_ERR(EBUSY);
        case NO_OBJECT:  return SYSCALL_ERR(EINVAL);
        default:         return SYSCALL_ERR(EIO);
    }
}
STATIC UINT64 Sys2KosSmpCountHandler(VOID) {
    return (UINT64)SmpGetCpuCount();
}

STATIC UINT64 Sys2KosSmpInfoHandler(UINT64 Index, UINT64 BufPtr, UINT64 Size) {
    Task *Cur = GetCurrentTask();
    if (!Cur || !Cur->IsUser) return SYSCALL_ERR(EPERM);
    if (!Cur->PageTable)      return SYSCALL_ERR(EFAULT);
    if (Size < sizeof(SmpCpuInfo)) return SYSCALL_ERR(EINVAL);
    if (!IsUserPointer((VOID *)BufPtr)) return SYSCALL_ERR(EFAULT);

    SmpCpuInfo Info;
    INT Rc = SmpGetCpuInfo((UINT32)Index, &Info);
    if (Rc != SUCCESS) {
        if (Rc == NOT_FOUND) return SYSCALL_ERR(ENOENT);
        return SYSCALL_ERR(EINVAL);
    }

    if (CopyToUser((VOID *)BufPtr, &Info, sizeof(Info)) != 0)
        return SYSCALL_ERR(EFAULT);

    return sizeof(Info);
}

STATIC VOID SyscallRegisterAll(VOID) {
    SyscallRegister(SYS_READ,       (SyscallFunction)SysReadHandler);
    SyscallRegister(SYS_WRITE,      (SyscallFunction)SysWriteHandler);
    SyscallRegister(SYS_OPEN,       (SyscallFunction)SysOpenHandler);
    SyscallRegister(SYS_CLOSE,      (SyscallFunction)SysCloseHandler);
    SyscallRegister(SYS_STAT,       (SyscallFunction)SysStatHandler);
    SyscallRegister(SYS_FSTAT,      (SyscallFunction)SysFstatHandler);
    SyscallRegister(SYS_LSEEK,      (SyscallFunction)SysLseekHandler);
    SyscallRegister(SYS_MMAP,       (SyscallFunction)SysMmapHandler);
    SyscallRegister(SYS_MUNMAP,     (SyscallFunction)SysMunmapHandler);
    SyscallRegister(SYS_BRK,        (SyscallFunction)SysBrkHandler);
    SyscallRegister(SYS_GETPID,     (SyscallFunction)SysGetPidHandler);
    SyscallRegister(SYS_EXIT,       (SyscallFunction)SysExitHandler);
    SyscallRegister(SYS_GETDENTS64, (SyscallFunction)SysGetdents64Handler);
    SyscallRegister(SYS_OPENAT,     (SyscallFunction)SysOpenAtHandler);
    SyscallRegister(SYS_NEWFSTATAT, (SyscallFunction)SysNewFstatAtHandler);
    SyscallRegister(SYS_GETCWD, (SyscallFunction)SysGetcwdHandler);
    SyscallRegister(SYS_CHDIR,  (SyscallFunction)SysChdirHandler);
    SyscallRegister(SYS_FCHDIR, (SyscallFunction)SysFchdirHandler);
    SyscallRegister(SYS_REBOOT, (SyscallFunction)SysRebootHandler);
    SyscallRegister(SYS_MKDIR,      (SyscallFunction)SysMkdirHandler);
    SyscallRegister(SYS_RMDIR,      (SyscallFunction)SysRmdirHandler);
    SyscallRegister(SYS_UNLINK,     (SyscallFunction)SysUnlinkHandler);
    SyscallRegister(SYS_MKDIRAT,    (SyscallFunction)SysMkdirAtHandler);
    SyscallRegister(SYS_UNLINKAT,   (SyscallFunction)SysUnlinkAtHandler);
    SyscallRegister(SYS_UMOUNT2, (SyscallFunction)SysUmount2Handler);
	SyscallRegister(SYS_2KOS_SMPCOUNT, (SyscallFunction)Sys2KosSmpCountHandler);
	SyscallRegister(SYS_2KOS_SMPINFO,  (SyscallFunction)Sys2KosSmpInfoHandler);

    SyscallRegister(SYS_2KOS_KOBJ_OPEN,    (SyscallFunction)Sys2KosKObjOpenHandler);
    SyscallRegister(SYS_2KOS_KOBJ_CLOSE,   (SyscallFunction)Sys2KosKObjCloseHandler);
    SyscallRegister(SYS_2KOS_KOBJ_READ,    (SyscallFunction)Sys2KosKObjReadHandler);
    SyscallRegister(SYS_2KOS_KOBJ_WRITE,   (SyscallFunction)Sys2KosKObjWriteHandler);
    SyscallRegister(SYS_2KOS_STDOUT_WRITE, (SyscallFunction)Sys2KosStdoutWriteHandler);
    SyscallRegister(SYS_2KOS_STDERR_WRITE, (SyscallFunction)Sys2KosStderrWriteHandler);
    SyscallRegister(SYS_2KOS_STDWARN_WRITE,(SyscallFunction)Sys2KosStdwarnWriteHandler);
    SyscallRegister(SYS_2KOS_STDIN_READ,   (SyscallFunction)Sys2KosStdinReadHandler);
    SyscallRegister(SYS_2KOS_GETTICKS,     (SyscallFunction)Sys2KosGetTicksHandler);
    SyscallRegister(SYS_2KOS_ELOG, (SyscallFunction)Sys2KosElogHandler);
    SyscallRegister(SYS_2KOS_SLOG, (SyscallFunction)Sys2KosSlogHandler);

    StdOutPrintf("syscall: %d handlers registered\n", GSyscallCount);
}

VOID SyscallInit(VOID) {
    StdOutPrintf("syscall: initializing...\n");
    
    // Clearing the table
    for (UINT64 I = 0; I < SYSCALL_MAX; I++) {
        GSyscallTable[I] = NULLPTR;
    }
    GSyscallCount = 0;
    
    // Register all syscalls
    SyscallRegisterAll();
    
    StdOutPrintf("syscall: initialization complete\n");
}

UINT64 SyscallHandler(UINT64 Num, UINT64 Arg1, UINT64 Arg2, UINT64 Arg3,
                      UINT64 Arg4, UINT64 Arg5, UINT64 Arg6) {
    UINT64 Args[6] = { Arg1, Arg2, Arg3, Arg4, Arg5, Arg6 };

    if (Num >= SYSCALL_MAX) {
        SyscallLogPush(Num, Args, SYSCALL_ERR(ENOSYS));
        return SYSCALL_ERR(ENOSYS);
    }

    SyscallFunction Func = GSyscallTable[Num];
    if (!Func) {
        SyscallLogPush(Num, Args, SYSCALL_ERR(ENOSYS));
        return SYSCALL_ERR(ENOSYS);
    }

    UINT64 Ret = Func(Arg1, Arg2, Arg3, Arg4, Arg5, Arg6);
    SyscallLogPush(Num, Args, Ret);
    return Ret;
}