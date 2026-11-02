#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <Scheduler.h>
#include <Io/StdIo.h>
#include <Basecon.h>
#include <Return.h>

#define STDIO_RING_SIZE   16384

typedef struct {
    UINT8  Buffer[STDIO_RING_SIZE];
    UINT32 Head;
    UINT32 Tail;
    UINT32 Count;
    SpinLock Lock;
} StdioRing;

typedef struct {
    StdioRing *Ring;
} StdioConsumerPriv;

STATIC KObj *GDefaultConsole = NULLPTR;

STATIC INT RingWrite(StdioRing *R, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    if (!R || !Buf || !Written) return INCORRECT_VALUE;
    *Written = 0;
    if (Size == 0) return SUCCESS;
    if (Size > STDIO_MSG_MAX) Size = STDIO_MSG_MAX;

    UINTPTR F = SpinLockAcquireIrq(&R->Lock);
    UINT32 N = 0;
    const UINT8 *In = (const UINT8*)Buf;
    while (N < Size && R->Count < STDIO_RING_SIZE) {
        R->Buffer[R->Head] = In[N++];
        R->Head = (R->Head + 1) % STDIO_RING_SIZE;
        R->Count++;
    }
    SpinLockReleaseIrq(&R->Lock, F);
    *Written = N;
    return SUCCESS;
}

STATIC INT RingRead(StdioRing *R, VOID *Buf, UINT32 Size, UINT32 *Read) {
    if (!R || !Buf || !Read) return INCORRECT_VALUE;
    *Read = 0;

    UINTPTR F = SpinLockAcquireIrq(&R->Lock);
    UINT32 N = 0;
    UINT8 *Out = (UINT8*)Buf;
    while (N < Size && R->Count > 0) {
        Out[N++] = R->Buffer[R->Tail];
        R->Tail = (R->Tail + 1) % STDIO_RING_SIZE;
        R->Count--;
    }
    SpinLockReleaseIrq(&R->Lock, F);
    *Read = N;
    return SUCCESS;
}

STATIC INT ConsoleReadOp(KObj *Obj, UINT64 Off, VOID *Buf, UINT32 Size, UINT32 *Read) {
    (VOID)Off;
    StdioConsumerPriv *P = (StdioConsumerPriv*)Obj->Private;
    if (!P || !P->Ring) return DEVICE_INVALID;
    return RingRead(P->Ring, Buf, Size, Read);
}

STATIC INT ConsoleWriteOp(KObj *Obj, UINT64 Off, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    (VOID)Off;
    StdioConsumerPriv *P = (StdioConsumerPriv*)Obj->Private;
    if (!P || !P->Ring) return DEVICE_INVALID;
    return RingWrite(P->Ring, Buf, Size, Written);
}

STATIC VOID ConsoleDestroyOp(KObj *Obj) {
    StdioConsumerPriv *P = (StdioConsumerPriv*)Obj->Private;
    if (P) {
        if (P->Ring) MemoryFree(P->Ring);
        MemoryFree(P);
        Obj->Private = NULLPTR;
    }
    if (Obj == GDefaultConsole) GDefaultConsole = NULLPTR;
}

STATIC KObjOperations GConsoleOps = {
    .Read    = ConsoleReadOp,
    .Write   = ConsoleWriteOp,
    .Destroy = ConsoleDestroyOp,
};

STATIC KObj *EnsureConsolesDir(VOID) {
    KObj *KernelDir = KObjFindByPath("\\Kernel");
    if (!KernelDir) {
        KernelDir = KObjCreate(KObjGetRoot(), "Kernel",
                               KOBJ_TYPE_DIRECTORY, NULLPTR, NULLPTR);
        if (!KernelDir) return NULLPTR;
    }
    KObj *Consoles = KObjFindByPath("\\Kernel\\Consoles");
    if (!Consoles) {
        Consoles = KObjCreate(KernelDir, "Consoles",
                              KOBJ_TYPE_DIRECTORY, NULLPTR, NULLPTR);
    }
    /* No KObjUnref(Consoles) — caller owns the reference. */
    KObjUnref(KernelDir);
    return Consoles;
}

KObj *StdIoCreateConsole(const CHAR *Name) {
    if (!Name || !Name[0]) return NULLPTR;
    CHAR Path[256];
    SnPrintf(Path, sizeof(Path), "\\Kernel\\Consoles\\%s", Name);
    KObj *Existing = KObjFindByPath(Path);
    if (Existing) return Existing;

    KObj *Consoles = EnsureConsolesDir();
    if (!Consoles) return NULLPTR;

    StdioRing *Ring = (StdioRing*)MemoryAllocate(sizeof(StdioRing));
    StdioConsumerPriv *P = (StdioConsumerPriv*)MemoryAllocate(sizeof(StdioConsumerPriv));
    if (!Ring || !P) {
        if (Ring) MemoryFree(Ring);
        if (P) MemoryFree(P);
        KObjUnref(Consoles);
        return NULLPTR;
    }
    MemSet(Ring, 0, sizeof(StdioRing));
    SpinLockInit(&Ring->Lock);
    P->Ring = Ring;

    KObj *Obj = KObjCreate(Consoles, Name, KOBJ_TYPE_STDIO, &GConsoleOps, P);
    KObjUnref(Consoles);
    if (!Obj) {
        MemoryFree(Ring);
        MemoryFree(P);
        return NULLPTR;
    }
    return Obj;
}

KObj *StdIoGetDefaultConsole(VOID) {
    if (!GDefaultConsole) return NULLPTR;
    KObjRef(GDefaultConsole);
    return GDefaultConsole;
}

VOID StdIoInit(VOID) {
    KObj *Basecon = StdIoCreateConsole("Basecon");
    if (!Basecon) {
        BaseconPrintf(BASECON_TYPE_ERROR, "stdio: cannot create Basecon\n");
        return;
    }
    if (!GDefaultConsole) GDefaultConsole = Basecon;
    else KObjUnref(Basecon);

    /* Now that GDefaultConsole is set, KLogPrint works. */
    KLogPrint("stdio: initialized\n");
}

STATIC KObj **ChannelSlot(Task *T, StdIoChannel Ch) {
    if (!T) return NULLPTR;
    switch (Ch) {
    case STDIO_CHAN_STDOUT:  return &T->StdoutObj;
    case STDIO_CHAN_STDERR:  return &T->StderrObj;
    case STDIO_CHAN_STDWARN: return &T->StdwarnObj;
    case STDIO_CHAN_STDIN:   return &T->StdinObj;
    default:                 return NULLPTR;
    }
}

INT StdIoBind(StdIoChannel Ch, KObj *Obj) {
    if (!Obj) return INCORRECT_VALUE;
    Task *Cur = GetCurrentTask();
    if (!Cur) return NO_OBJECT;
    KObj **Slot = ChannelSlot(Cur, Ch);
    if (!Slot) return INCORRECT_VALUE;
    KObjRef(Obj);
    if (*Slot) KObjUnref(*Slot);
    *Slot = Obj;
    return SUCCESS;
}

VOID StdIoUnbind(StdIoChannel Ch) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return;
    KObj **Slot = ChannelSlot(Cur, Ch);
    if (!Slot) return;
    if (*Slot) { KObjUnref(*Slot); *Slot = NULLPTR; }
}

KObj *StdIoGetBinding(StdIoChannel Ch) {
    Task *Cur = GetCurrentTask();
    if (!Cur) return NULLPTR;
    KObj **Slot = ChannelSlot(Cur, Ch);
    if (!Slot || !*Slot) return NULLPTR;
    KObjRef(*Slot);
    return *Slot;
}

STATIC KObj *ResolveChannel(StdIoChannel Ch) {
    KObj *Con = StdIoGetBinding(Ch);
    if (Con) return Con;
    return StdIoGetDefaultConsole();
}

INT StdIoWriteTo(KObj *Con, const VOID *Buf, UINT32 Size) {
    if (!Con || !Buf) return INCORRECT_VALUE;
    if (!Con->Ops || !Con->Ops->Write) return NOT_SUPPORTED;
    UINT32 W = 0;
    INT Rc = Con->Ops->Write(Con, 0, Buf, Size, &W);
    return Rc == SUCCESS ? (INT)W : Rc;
}

INT StdIoPrintTo(KObj *Con, const CHAR *Str) {
    if (!Str) return INCORRECT_VALUE;
    return StdIoWriteTo(Con, Str, (UINT32)StrLen(Str));
}

INT StdIoPrintfTo(KObj *Con, const CHAR *Fmt, ...) {
    if (!Fmt || !Con) return INCORRECT_VALUE;
    CHAR Buf[STDIO_MSG_MAX];
    VA_LIST Args;
    VaStart(Args, Fmt);
    INT N = VsnPrintf(Buf, sizeof(Buf), Fmt, Args);
    VaEnd(Args);
    if (N <= 0) return N;
    StdIoWriteTo(Con, Buf, (UINT32)N);
    return N;
}

INT StdIoReadFrom(KObj *Con, VOID *Buf, UINT32 Size, UINT32 *Read) {
    if (!Con || !Buf || !Read) return INCORRECT_VALUE;
    if (!Con->Ops || !Con->Ops->Read) return NOT_SUPPORTED;
    return Con->Ops->Read(Con, 0, Buf, Size, Read);
}

INT StdIoWriteChannel(StdIoChannel Ch, const VOID *Buf, UINT32 Size) {
    KObj *Con = ResolveChannel(Ch);
    if (!Con) return NOT_FOUND;
    INT Rc = StdIoWriteTo(Con, Buf, Size);
    KObjUnref(Con);
    return Rc;
}

INT StdIoPrintChannel(StdIoChannel Ch, const CHAR *Str) {
    if (!Str) return INCORRECT_VALUE;
    return StdIoWriteChannel(Ch, Str, (UINT32)StrLen(Str));
}

INT StdIoPrintfChannel(StdIoChannel Ch, const CHAR *Fmt, ...) {
    if (!Fmt) return INCORRECT_VALUE;
    CHAR Buf[STDIO_MSG_MAX];
    VA_LIST Args;
    VaStart(Args, Fmt);
    INT N = VsnPrintf(Buf, sizeof(Buf), Fmt, Args);
    VaEnd(Args);
    if (N <= 0) return N;
    StdIoWriteChannel(Ch, Buf, (UINT32)N);
    return N;
}

INT StdIoReadChannel(StdIoChannel Ch, VOID *Buf, UINT32 Size, UINT32 *Read) {
    KObj *Con = ResolveChannel(Ch);
    if (!Con) return NOT_FOUND;
    INT Rc = StdIoReadFrom(Con, Buf, Size, Read);
    KObjUnref(Con);
    return Rc;
}

INT KLogPrint(const CHAR *Str) {
    if (!Str) return INCORRECT_VALUE;
    KObj *Con = StdIoGetDefaultConsole();
    if (!Con) return NOT_FOUND;
    INT Rc = StdIoPrintTo(Con, Str);
    KObjUnref(Con);
    return Rc;
}

INT KLogPrintf(const CHAR *Fmt, ...) {
    if (!Fmt) return INCORRECT_VALUE;
    CHAR Buf[STDIO_MSG_MAX];
    VA_LIST Args;
    VaStart(Args, Fmt);
    INT N = VsnPrintf(Buf, sizeof(Buf), Fmt, Args);
    VaEnd(Args);
    if (N <= 0) return N;
    KLogPrint(Buf);
    return N;
}