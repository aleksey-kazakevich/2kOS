#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Fs/FdTable.h>
#include <Errno.h>

VOID FdTableInit(FdTable *T) {
    if (!T) return;
    MemSet(T, 0, sizeof(FdTable));
    T->Count = 0;
}

STATIC VOID FdEntryClose(FdEntry *E) {
    if (!E || E->Kind == FD_KIND_FREE) return;

    if (E->Kind == FD_KIND_VFS) {
        if (E->File) {
            VfsClose(E->File);
            E->File = NULLPTR;
        }
    } else if (E->Kind == FD_KIND_KOBJ) {
        if (E->Obj) {
            KObjUnref(E->Obj);
            E->Obj = NULLPTR;
        }
    }

    E->Kind = FD_KIND_FREE;
    E->Flags = 0;
    E->Offset = 0;
    E->RefCount = 0;
}

VOID FdTableCloseAll(FdTable *T) {
    if (!T) return;
    for (UINT32 I = 0; I < FD_MAX; I++) {
        if (T->Entries[I].Kind != FD_KIND_FREE) {
            FdEntryClose(&T->Entries[I]);
        }
    }
    T->Count = 0;
}

INT FdAllocFrom(FdTable *T, UINT32 MinFd) {
    if (!T) return -EINVAL;
    if (MinFd >= FD_MAX) return -EMFILE;

    for (UINT32 I = MinFd; I < FD_MAX; I++) {
        if (T->Entries[I].Kind == FD_KIND_FREE) {
            T->Count++;
            return (INT)I;
        }
    }
    return -EMFILE;
}

INT FdAlloc(FdTable *T) {
    return FdAllocFrom(T, FD_FIRST_USER);
}

VOID FdFree(FdTable *T, UINT32 Fd) {
    if (!T || Fd >= FD_MAX) return;
    if (T->Entries[Fd].Kind == FD_KIND_FREE) return;
    FdEntryClose(&T->Entries[Fd]);
    if (T->Count > 0) T->Count--;
}

FdEntry *FdGet(FdTable *T, UINT32 Fd) {
    if (!T || Fd >= FD_MAX) return NULLPTR;
    if (T->Entries[Fd].Kind == FD_KIND_FREE) return NULLPTR;
    return &T->Entries[Fd];
}

INT FdInstallVfs(FdTable *T, VfsFile *File, UINT32 Flags) {
    if (!T || !File) return -EINVAL;

    INT Fd = FdAlloc(T);
    if (Fd < 0) return Fd;

    FdEntry *E = &T->Entries[Fd];
    E->Kind   = FD_KIND_VFS;
    E->File   = File;
    E->Flags  = Flags;
    E->Offset = 0;
    E->RefCount = 1;
    return Fd;
}

INT FdInstallKObj(FdTable *T, KObj *Obj, UINT32 Flags) {
    if (!T || !Obj) return -EINVAL;

    INT Fd = FdAlloc(T);
    if (Fd < 0) return Fd;

    FdEntry *E = &T->Entries[Fd];
    E->Kind   = FD_KIND_KOBJ;
    E->Obj    = Obj;
    E->Flags  = Flags;
    E->Offset = 0;
    E->RefCount = 1;
    return Fd;
}

VfsFile *FdGetVfs(FdTable *T, UINT32 Fd) {
    FdEntry *E = FdGet(T, Fd);
    if (!E || E->Kind != FD_KIND_VFS) return NULLPTR;
    return E->File;
}

KObj *FdGetKObj(FdTable *T, UINT32 Fd) {
    FdEntry *E = FdGet(T, Fd);
    if (!E || E->Kind != FD_KIND_KOBJ) return NULLPTR;
    return E->Obj;
}

VOID FdSetupStdio(FdTable *T, KObj *In, KObj *Out, KObj *Err) {
    if (!T) return;

    if (In) {
        T->Entries[0].Kind     = FD_KIND_KOBJ;
        T->Entries[0].Obj      = In;
        T->Entries[0].Flags    = O_RDONLY;   /* Linux: 0 */
        T->Entries[0].RefCount = 1;
        KObjRef(In);
    }

    if (Out) {
        T->Entries[1].Kind     = FD_KIND_KOBJ;
        T->Entries[1].Obj      = Out;
        T->Entries[1].Flags    = O_WRONLY;   /* Linux: 1 */
        T->Entries[1].RefCount = 1;
        KObjRef(Out);
    }

    if (Err) {
        T->Entries[2].Kind     = FD_KIND_KOBJ;
        T->Entries[2].Obj      = Err;
        T->Entries[2].Flags    = O_WRONLY;   /* Linux: 1 */
        T->Entries[2].RefCount = 1;
        KObjRef(Err);
    }
}