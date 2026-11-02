#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
#include <SpinLock.h>
#include <SysDevice.h>
#include <Basecon.h>
#include <Return.h>

STATIC ListHead GSysDeviceList;
STATIC UINT32   GNextDeviceId = 1;
STATIC UINT32   GDeviceCount = 0;
STATIC SysDevice *GRootDevice = NULLPTR;
STATIC SpinLock GSysDevLock;

// ============================================================================
// LOCKED INTERNAL — it is assumed that GSysDevLock has already been taken
// ============================================================================

STATIC VOID SysDeviceAddToListLocked(SysDevice *Dev) {
    ListAddTail(&GSysDeviceList, &Dev->Node);
    GDeviceCount++;
}

STATIC VOID SysDeviceRemoveFromListLocked(SysDevice *Dev) {
    ListDel(&Dev->Node);
    if (GDeviceCount > 0) GDeviceCount--;
}

STATIC VOID SysDeviceAddToParentLocked(SysDevice *Dev, SysDevice *Parent) {
    if (!Parent) return;
    Dev->Parent = Parent;
    ListAddTail(&Parent->Children, &Dev->SiblingNode);
    Parent->ChildCount++;
}

STATIC VOID SysDeviceRemoveFromParentLocked(SysDevice *Dev) {
    if (!Dev->Parent) return;
    ListDel(&Dev->SiblingNode);
    if (Dev->Parent->ChildCount > 0) Dev->Parent->ChildCount--;
    Dev->Parent = NULLPTR;
}

STATIC SysDevice *SysDeviceAllocLocked(VOID) {
    SysDevice *Dev = (SysDevice *)MemoryAllocate(sizeof(SysDevice));
    if (!Dev) return NULLPTR;

    MemSet(Dev, 0, sizeof(SysDevice));
    ListInit(&Dev->Children);

    Dev->Id = GNextDeviceId++;
    Dev->State = SYSDEV_STATE_UNKNOWN;
    Dev->AddTick = TimerTicks();

    return Dev;
}

STATIC VOID SysDeviceDestroyRecursiveLocked(SysDevice *Dev) {
    // Childrens
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &Dev->Children) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        SysDeviceDestroyRecursiveLocked(Child);
    }

    // Unlink the driver (just reset the field, without calling external ones)
    Dev->Driver = NULLPTR;

    SysDeviceRemoveFromParentLocked(Dev);
    SysDeviceRemoveFromListLocked(Dev);

    if (GRootDevice == Dev) GRootDevice = NULLPTR;

    MemoryFree(Dev);
}

STATIC VOID SysDeviceWalkRecursiveLocked(SysDevice *Dev,
                                          SysDeviceVisitor Visitor,
                                          VOID *UserData) {
    if (!Dev) return;
    Visitor(Dev, UserData);

    ListHead *Pos;
    ListForEach(Pos, &Dev->Children) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        SysDeviceWalkRecursiveLocked(Child, Visitor, UserData);
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

VOID SysDeviceInit(VOID) {
    ListInit(&GSysDeviceList);
    GNextDeviceId = 1;
    GDeviceCount = 0;
    GRootDevice = NULLPTR;
    SpinLockInit(&GSysDevLock);
}

SysDevice *SysDeviceCreateRoot(const CHAR *Name) {
    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);

    if (GRootDevice) {
        SpinLockReleaseIrq(&GSysDevLock, Flags);
        return GRootDevice;
    }

    SysDevice *Root = SysDeviceAllocLocked();
    if (!Root) {
        SpinLockReleaseIrq(&GSysDevLock, Flags);
        return NULLPTR;
    }

    StrnCpy(Root->Name, Name ? Name : "root", SYSDEV_NAME_MAX - 1);
    StrnCpy(Root->Description, "Root device", SYSDEV_NAME_MAX - 1);
    Root->Type = SYSDEV_TYPE_ROOT;
    Root->State = SYSDEV_STATE_ENABLED;

    SysDeviceAddToListLocked(Root);
    GRootDevice = Root;

    SpinLockReleaseIrq(&GSysDevLock, Flags);
    return Root;
}

SysDevice *SysDeviceCreate(SysDevice *Parent,
                           const CHAR *Name,
                           const CHAR *Description,
                           SysDeviceType Type) {
    if (!Name) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);

    SysDevice *Dev = SysDeviceAllocLocked();
    if (!Dev) {
        SpinLockReleaseIrq(&GSysDevLock, Flags);
        return NULLPTR;
    }

    StrnCpy(Dev->Name, Name, SYSDEV_NAME_MAX - 1);
    StrnCpy(Dev->Description, Description ? Description : Name,
            SYSDEV_NAME_MAX - 1);
    Dev->Type = Type;
    Dev->State = SYSDEV_STATE_PRESENT;

    SysDeviceAddToListLocked(Dev);
    SysDeviceAddToParentLocked(Dev, Parent);

    SpinLockReleaseIrq(&GSysDevLock, Flags);
    return Dev;
}

VOID SysDeviceDestroy(SysDevice *Dev) {
    if (!Dev) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);
    SysDeviceDestroyRecursiveLocked(Dev);
    SpinLockReleaseIrq(&GSysDevLock, Flags);
}

SysDevice *SysDeviceFind(const CHAR *Name) {
    if (!Name) return NULLPTR;

    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);

    SysDevice *Result = NULLPTR;
    ListHead *Pos;
    ListForEach(Pos, &GSysDeviceList) {
        SysDevice *Dev = ListEntry(Pos, SysDevice, Node);
        if (StrCmp(Dev->Name, Name) == 0) {
            Result = Dev;
            break;
        }
    }

    SpinLockReleaseIrq(&GSysDevLock, Flags);
    return Result;
}

SysDevice *SysDeviceFindById(UINT32 Id) {
    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);

    SysDevice *Result = NULLPTR;
    ListHead *Pos;
    ListForEach(Pos, &GSysDeviceList) {
        SysDevice *Dev = ListEntry(Pos, SysDevice, Node);
        if (Dev->Id == Id) {
            Result = Dev;
            break;
        }
    }

    SpinLockReleaseIrq(&GSysDevLock, Flags);
    return Result;
}

VOID SysDeviceWalkFrom(SysDevice *Root, SysDeviceVisitor Visitor, VOID *UserData) {
    if (!Root || !Visitor) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);
    SysDeviceWalkRecursiveLocked(Root, Visitor, UserData);
    SpinLockReleaseIrq(&GSysDevLock, Flags);
}

VOID SysDeviceWalk(SysDeviceVisitor Visitor, VOID *UserData) {
    if (!Visitor) return;

    UINTPTR Flags = SpinLockAcquireIrq(&GSysDevLock);

    ListHead *Pos;
    ListForEach(Pos, &GSysDeviceList) {
        SysDevice *Dev = ListEntry(Pos, SysDevice, Node);
        if (Dev->Parent) continue;
        SysDeviceWalkRecursiveLocked(Dev, Visitor, UserData);
    }

    SpinLockReleaseIrq(&GSysDevLock, Flags);
}

const CHAR *SysDeviceStateToString(SysDeviceState State) {
    switch (State) {
        case SYSDEV_STATE_UNKNOWN:   return "unknown";
        case SYSDEV_STATE_PRESENT:   return "present";
        case SYSDEV_STATE_ENABLED:   return "enabled";
        case SYSDEV_STATE_DISABLED:  return "disabled";
        case SYSDEV_STATE_ERROR:     return "error";
        case SYSDEV_STATE_REMOVED:   return "removed";
        default:                     return "?";
    }
}

const CHAR *SysDeviceTypeToString(SysDeviceType Type) {
    switch (Type) {
        case SYSDEV_TYPE_ROOT:        return "root";
        case SYSDEV_TYPE_BUS:         return "bus";
        case SYSDEV_TYPE_CONTROLLER:  return "controller";
        case SYSDEV_TYPE_STORAGE:     return "storage";
        case SYSDEV_TYPE_NETWORK:     return "network";
        case SYSDEV_TYPE_DISPLAY:     return "display";
        case SYSDEV_TYPE_SOUND:       return "sound";
        case SYSDEV_TYPE_INPUT:       return "input";
        case SYSDEV_TYPE_SERIAL:      return "serial";
        case SYSDEV_TYPE_TIMER:       return "timer";
        case SYSDEV_TYPE_INTERRUPT:   return "interrupt";
        case SYSDEV_TYPE_POWER:       return "power";
        case SYSDEV_TYPE_VIRTUAL:     return "virtual";
        case SYSDEV_TYPE_CPU:         return "cpu";
        case SYSDEV_TYPE_CORE:        return "core";
        default:                      return "unknown";
    }
}

VOID SysDeviceSetState(SysDevice *Dev, SysDeviceState State) {
    if (!Dev) return;
    Dev->State = State;
}

INT SysDeviceBindDriver(SysDevice *Dev, KDriver *Driver) {
    if (!Dev || !Driver) RETURN(NO_OBJECT);
    if (Dev->Driver) RETURN(ALREADY_EXISTS);

    Dev->Driver = Driver;
    Dev->State = SYSDEV_STATE_ENABLED;

    RETURN(SUCCESS);
}

INT SysDeviceUnbindDriver(SysDevice *Dev) {
    if (!Dev) RETURN(NO_OBJECT);
    if (!Dev->Driver) RETURN(NO_OBJECT);

    Dev->Driver = NULLPTR;
    Dev->State = SYSDEV_STATE_DISABLED;

    RETURN(SUCCESS);
}

KDriver *SysDeviceGetDriver(SysDevice *Dev) {
    return Dev ? Dev->Driver : NULLPTR;
}

INT SysDeviceAddResource(SysDevice *Dev, SysDeviceResType Type,
                         UINT64 Base, UINT64 Size) {
    if (!Dev) RETURN(NO_OBJECT);
    if (Dev->ResourceCount >= SYSDEV_MAX_RESOURCES) RETURN(NO_MEMORY);

    SysDeviceResource *R = &Dev->Resources[Dev->ResourceCount++];
    R->Type = Type;
    R->Base = Base;
    R->Size = Size;
    R->Flags = 0;

    RETURN(SUCCESS);
}

INT SysDeviceFindResource(SysDevice *Dev, SysDeviceResType Type) {
    if (!Dev) return -1;
    for (UINT32 I = 0; I < Dev->ResourceCount; I++) {
        if (Dev->Resources[I].Type == Type) return (INT)I;
    }
    return -1;
}

const CHAR *SysDeviceResTypeToString(SysDeviceResType Type) {
    switch (Type) {
        case SYSDEV_RES_IRQ:   return "irq";
        case SYSDEV_RES_MMIO:  return "mmio";
        case SYSDEV_RES_IO:    return "io";
        case SYSDEV_RES_DMA:   return "dma";
        case SYSDEV_RES_MEM:   return "mem";
        case SYSDEV_RES_BUS:   return "bus";
        default:               return "?";
    }
}

STATIC SysDeviceProp *SysDeviceFindProp(SysDevice *Dev, const CHAR *Name) {
    for (UINT32 I = 0; I < Dev->PropCount; I++) {
        if (StrCaseCmp(Dev->Properties[I].Name, Name) == 0) {
            return &Dev->Properties[I];
        }
    }
    return NULLPTR;
}

VOID SysDeviceSetProp(SysDevice *Dev, const CHAR *Name, const CHAR *Value) {
    if (!Dev || !Name || !Value) return;

    SysDeviceProp *P = SysDeviceFindProp(Dev, Name);
    if (!P) {
        if (Dev->PropCount >= SYSDEV_MAX_PROPS) return;
        P = &Dev->Properties[Dev->PropCount++];
        StrnCpy(P->Name, Name, SYSDEV_PROP_NAME_MAX - 1);
    }

    StrnCpy(P->Value, Value, SYSDEV_PROP_VALUE_MAX - 1);
}

VOID SysDeviceSetPropU32(SysDevice *Dev, const CHAR *Name, UINT32 Value) {
    CHAR Buf[SYSDEV_PROP_VALUE_MAX];
    SnPrintf(Buf, sizeof(Buf), "%u", Value);
    SysDeviceSetProp(Dev, Name, Buf);
}

VOID SysDeviceSetPropU64(SysDevice *Dev, const CHAR *Name, UINT64 Value) {
    CHAR Buf[SYSDEV_PROP_VALUE_MAX];
    SnPrintf(Buf, sizeof(Buf), "%llu", Value);
    SysDeviceSetProp(Dev, Name, Buf);
}

VOID SysDeviceSetPropHex(SysDevice *Dev, const CHAR *Name, UINT64 Value) {
    CHAR Buf[SYSDEV_PROP_VALUE_MAX];
    SnPrintf(Buf, sizeof(Buf), "0x%llX", Value);
    SysDeviceSetProp(Dev, Name, Buf);
}

const CHAR *SysDeviceGetProp(SysDevice *Dev, const CHAR *Name) {
    if (!Dev || !Name) return NULLPTR;
    SysDeviceProp *P = SysDeviceFindProp(Dev, Name);
    return P ? P->Value : NULLPTR;
}

// ============================================================================
// QUERIES
// ============================================================================

UINT32 SysDeviceGetCount(VOID) {
    // Atomic read — GDeviceCount changes only under GSysDevLock,
    // but reading a 32-bit value is atomic on x86_64.
    return GDeviceCount;
}

SysDevice *SysDeviceGetRoot(VOID) {
    return GRootDevice;
}

VOID SysDeviceGetPath(SysDevice *Dev, CHAR *Buf, USIZE BufSize) {
    if (!Dev || !Buf || BufSize == 0) return;

    SysDevice *Stack[32];
    INT Depth = 0;
    SysDevice *Cur = Dev;

    while (Cur && Depth < 32) {
        Stack[Depth++] = Cur;
        Cur = Cur->Parent;
    }

    Buf[0] = '\0';
    for (INT I = Depth - 1; I >= 0; I--) {
        if (I < Depth - 1) {
            StrnCat(Buf, "/", BufSize - StrLen(Buf) - 1);
        }
        StrnCat(Buf, Stack[I]->Name, BufSize - StrLen(Buf) - 1);
    }
}