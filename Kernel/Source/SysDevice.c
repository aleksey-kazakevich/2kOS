#include <Mem/Allocator.h>
#include <Lib/String.h>
#include <Time/Timer.h>
#include <Lib/StdIo.h>
#include <SysDevice.h>
#include <Basecon.h>
#include <Return.h>

STATIC ListHead GSysDeviceList;
STATIC UINT32   GNextDeviceId = 1;
STATIC UINT32   GDeviceCount = 0;
STATIC SysDevice *GRootDevice = NULLPTR;

VOID SysDeviceInit(VOID) {
    ListInit(&GSysDeviceList);
    GNextDeviceId = 1;
    GDeviceCount = 0;
    GRootDevice = NULLPTR;
}

STATIC VOID SysDeviceAddToList(SysDevice *Dev) {
    ListAddTail(&GSysDeviceList, &Dev->Node);
    GDeviceCount++;
}

STATIC VOID SysDeviceRemoveFromList(SysDevice *Dev) {
    ListDel(&Dev->Node);
    if (GDeviceCount > 0) GDeviceCount--;
}

STATIC VOID SysDeviceAddToParent(SysDevice *Dev, SysDevice *Parent) {
    if (!Parent) return;
    Dev->Parent = Parent;
    ListAddTail(&Parent->Children, &Dev->SiblingNode);
    Parent->ChildCount++;
}

STATIC VOID SysDeviceRemoveFromParent(SysDevice *Dev) {
    if (!Dev->Parent) return;
    ListDel(&Dev->SiblingNode);
    if (Dev->Parent->ChildCount > 0) Dev->Parent->ChildCount--;
    Dev->Parent = NULLPTR;
}

STATIC SysDevice *SysDeviceAlloc(VOID) {
    SysDevice *Dev = (SysDevice *)MemoryAllocate(sizeof(SysDevice));
    if (!Dev) return NULLPTR;

    MemSet(Dev, 0, sizeof(SysDevice));
    ListInit(&Dev->Children);

    Dev->Id = GNextDeviceId++;
    Dev->State = SYSDEV_STATE_UNKNOWN;
    Dev->AddTick = TimerTicks();

    return Dev;
}

SysDevice *SysDeviceCreateRoot(const CHAR *Name) {
    if (GRootDevice) return GRootDevice;

    SysDevice *Root = SysDeviceAlloc();
    if (!Root) return NULLPTR;

    StrnCpy(Root->Name, Name ? Name : "root", SYSDEV_NAME_MAX - 1);
    StrnCpy(Root->Description, "Root device", SYSDEV_NAME_MAX - 1);
    Root->Type = SYSDEV_TYPE_ROOT;
    Root->State = SYSDEV_STATE_ENABLED;

    SysDeviceAddToList(Root);
    GRootDevice = Root;

    return Root;
}

SysDevice *SysDeviceCreate(SysDevice *Parent,
                           const CHAR *Name,
                           const CHAR *Description,
                           SysDeviceType Type) {
    if (!Name) return NULLPTR;

    SysDevice *Dev = SysDeviceAlloc();
    if (!Dev) return NULLPTR;

    StrnCpy(Dev->Name, Name, SYSDEV_NAME_MAX - 1);
    StrnCpy(Dev->Description, Description ? Description : Name,
            SYSDEV_NAME_MAX - 1);
    Dev->Type = Type;
    Dev->State = SYSDEV_STATE_PRESENT;

    SysDeviceAddToList(Dev);
    SysDeviceAddToParent(Dev, Parent);

    return Dev;
}

STATIC VOID SysDeviceDestroyRecursive(SysDevice *Dev) {
    // Сначала детей
    ListHead *Pos, *Next;
    ListForEachSafe(Pos, Next, &Dev->Children) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        SysDeviceDestroyRecursive(Child);
    }

    // Отвязываем драйвер
    if (Dev->Driver) {
        SysDeviceUnbindDriver(Dev);
    }

    // Убираем из структур
    SysDeviceRemoveFromParent(Dev);
    SysDeviceRemoveFromList(Dev);

    if (GRootDevice == Dev) GRootDevice = NULLPTR;

    MemoryFree(Dev);
}

VOID SysDeviceDestroy(SysDevice *Dev) {
    if (!Dev) return;
    SysDeviceDestroyRecursive(Dev);
}

SysDevice *SysDeviceFind(const CHAR *Name) {
    if (!Name) return NULLPTR;

    ListHead *Pos;
    ListForEach(Pos, &GSysDeviceList) {
        SysDevice *Dev = ListEntry(Pos, SysDevice, Node);
        if (StrCmp(Dev->Name, Name) == 0) return Dev;
    }
    return NULLPTR;
}

SysDevice *SysDeviceFindById(UINT32 Id) {
    ListHead *Pos;
    ListForEach(Pos, &GSysDeviceList) {
        SysDevice *Dev = ListEntry(Pos, SysDevice, Node);
        if (Dev->Id == Id) return Dev;
    }
    return NULLPTR;
}

STATIC VOID SysDeviceWalkRecursive(SysDevice *Dev,
                                   SysDeviceVisitor Visitor,
                                   VOID *UserData) {
    if (!Dev) return;
    Visitor(Dev, UserData);

    ListHead *Pos;
    ListForEach(Pos, &Dev->Children) {
        SysDevice *Child = ListEntry(Pos, SysDevice, SiblingNode);
        SysDeviceWalkRecursive(Child, Visitor, UserData);
    }
}

VOID SysDeviceWalkFrom(SysDevice *Root, SysDeviceVisitor Visitor, VOID *UserData) {
    if (!Root || !Visitor) return;
    SysDeviceWalkRecursive(Root, Visitor, UserData);
}

STATIC VOID SysDeviceWalkTopLevel(SysDevice *Dev, VOID *UserData) {
    (VOID)UserData;
    SysDeviceWalkRecursive(Dev, (SysDeviceVisitor)UserData, NULLPTR);
}

VOID SysDeviceWalk(SysDeviceVisitor Visitor, VOID *UserData) {
    if (!Visitor) return;

    // Проходим по всем устройствам (top-level: у кого Parent == NULL)
    ListHead *Pos;
    ListForEach(Pos, &GSysDeviceList) {
        SysDevice *Dev = ListEntry(Pos, SysDevice, Node);
        if (Dev->Parent) continue;   // это не корень
        SysDeviceWalkRecursive(Dev, Visitor, UserData);
    }
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

UINT32 SysDeviceGetCount(VOID) {
    return GDeviceCount;
}

SysDevice *SysDeviceGetRoot(VOID) {
    return GRootDevice;
}

VOID SysDeviceGetPath(SysDevice *Dev, CHAR *Buf, USIZE BufSize) {
    if (!Dev || !Buf || BufSize == 0) return;

    // Собираем путь от корня к Dev
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