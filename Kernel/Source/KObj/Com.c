#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Lib/StdIo.h>
#include <KObj/Com.h>
#include <Basecon.h>
#include <Return.h>

#define KOBJ_COM_MAX 4

STATIC KObj *GComObjs[KOBJ_COM_MAX] = {0};

STATIC INT ComKObjReadOp(KObj *Obj, UINT64 Off, VOID *Buf, UINT32 Size, UINT32 *Read) {
    (VOID)Off;
    if (!Obj || !Obj->Private || !Buf || !Read) return INCORRECT_VALUE;

    ComDevice *Dev = (ComDevice*)Obj->Private;
    UINT8 *Out = (UINT8*)Buf;
    UINT32 N = 0;

    while (N < Size && ComDataAvailable(Dev)) {
        Out[N++] = (UINT8)ComGetChar(Dev);
    }
    *Read = N;
    return SUCCESS;
}

STATIC INT ComKObjWriteOp(KObj *Obj, UINT64 Off, const VOID *Buf, UINT32 Size, UINT32 *Written) {
    (VOID)Off;
    if (!Obj || !Obj->Private || !Buf || !Written) return INCORRECT_VALUE;

    ComDevice *Dev = (ComDevice*)Obj->Private;
    const UINT8 *In = (const UINT8*)Buf;

    for (UINT32 I = 0; I < Size; I++) {
        if (In[I] == '\n') ComPutChar(Dev, '\r');
        ComPutChar(Dev, (CHAR)In[I]);
    }
    ComFlush(Dev);

    *Written = Size;
    return SUCCESS;
}

STATIC VOID ComKObjDestroyOp(KObj *Obj) {
    if (!Obj) return;
    /* Private = ComDevice*, owned by the driver — do NOT free. */
    Obj->Private = NULLPTR;

    for (UINT32 I = 0; I < KOBJ_COM_MAX; I++) {
        if (GComObjs[I] == Obj) GComObjs[I] = NULLPTR;
    }
}

STATIC KObjOperations GComKObjOps = {
    .Read    = ComKObjReadOp,
    .Write   = ComKObjWriteOp,
    .Destroy = ComKObjDestroyOp,
};

STATIC KObj *EnsureDeviceDir(VOID) {
    KObj *Dev = KObjFindByPath("\\Device");
    if (!Dev) {
        Dev = KObjCreate(KObjGetRoot(), "Device",
                         KOBJ_TYPE_DIRECTORY, NULLPTR, NULLPTR);
    }
    return Dev;
}

VOID KObjComRegisterPort(ComDevice *Dev, UINT8 Index) {
    if (!Dev || Index >= KOBJ_COM_MAX) return;
    if (GComObjs[Index]) return;   /* already registered */

    KObj *DevDir = EnsureDeviceDir();
    if (!DevDir) {
        BaseconPrintf(BASECON_TYPE_ERROR, "com: cannot create \\Device\n");
        return;
    }

    CHAR Name[16];
    SnPrintf(Name, sizeof(Name), "COM%u", Index);

    KObj *Obj = KObjCreate(DevDir, Name, KOBJ_TYPE_UNKNOWN, &GComKObjOps, Dev);
    KObjUnref(DevDir);

    if (!Obj) {
        BaseconPrintf(BASECON_TYPE_ERROR, "com: cannot create \\Device\\%s\n", Name);
        return;
    }
    Obj->Access = KOBJ_ACCESS_BOTH;
    GComObjs[Index] = Obj;
}

KObj *KObjComGet(UINT8 Index) {
    if (Index >= KOBJ_COM_MAX) return NULLPTR;
    if (!GComObjs[Index]) return NULLPTR;
    KObjRef(GComObjs[Index]);
    return GComObjs[Index];
}