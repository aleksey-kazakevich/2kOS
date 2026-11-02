#pragma once

#include <KDriver.h>
#include <List.h>
#include <Types.h>

#define SYSDEV_NAME_MAX         64
#define SYSDEV_PROP_NAME_MAX    32
#define SYSDEV_PROP_VALUE_MAX   128
#define SYSDEV_MAX_RESOURCES    8
#define SYSDEV_MAX_PROPS        16

typedef enum {
    SYSDEV_TYPE_UNKNOWN = 0,
    SYSDEV_TYPE_ROOT,           // the root of the tree
    SYSDEV_TYPE_BUS,            // the bus (PCI, USB, ISA)
    SYSDEV_TYPE_CONTROLLER,     // the controller (IDE, SATA, USB-HC, FDC)
    SYSDEV_TYPE_STORAGE,        // disk, flash drive
    SYSDEV_TYPE_NETWORK,        // network adapter
    SYSDEV_TYPE_DISPLAY,        // video card
    SYSDEV_TYPE_SOUND,          // sound
    SYSDEV_TYPE_INPUT,          // keyboard, mouse
    SYSDEV_TYPE_SERIAL,         // COM, USB-Serial
    SYSDEV_TYPE_TIMER,          // HPET, PIT, APIC timer
    SYSDEV_TYPE_INTERRUPT,      // APIC, IOAPIC, PIC
    SYSDEV_TYPE_POWER,          // ACPI, PMC
    SYSDEV_TYPE_CPU,            // processor
    SYSDEV_TYPE_CORE,           // processor core
    SYSDEV_TYPE_VIRTUAL,        // virtual device (loopback, null)
} SysDeviceType;

typedef enum {
    SYSDEV_STATE_UNKNOWN = 0,
    SYSDEV_STATE_PRESENT,       // detected, the driver is not linked yet
    SYSDEV_STATE_ENABLED,       // the driver is linked and running
    SYSDEV_STATE_DISABLED,      // disabled manually
    SYSDEV_STATE_ERROR,         // initialization error
    SYSDEV_STATE_REMOVED,       // deleted (hot-plug)
} SysDeviceState;

typedef enum {
    SYSDEV_RES_IRQ = 0,         // IRQ / GSI line / vector
    SYSDEV_RES_MMIO,            // memory-mapped I/O
    SYSDEV_RES_IO,              // I/O ports (start/end)
    SYSDEV_RES_DMA,             // DMA channel
    SYSDEV_RES_MEM,             // memory (buffers, windows)
    SYSDEV_RES_BUS,             // bus range (PCI bus range)
} SysDeviceResType;

typedef struct {
    SysDeviceResType Type;

    // For IRQ: Base = IRQ/GSI, Size = vector
    // For MMIO: Base = phys addr, Size = length
    // For IO: Base = start port, Size = number of ports
    // For DMA: Base = channel, Size = 1
    // For MEM: Base = phys addr, Size = length
    // For BUS: Base = start bus, Size = count
    UINT64 Base;
    UINT64 Size;

    UINT32 Flags;               // reserved for the future
} SysDeviceResource;

typedef struct {
    CHAR Name[SYSDEV_PROP_NAME_MAX];
    CHAR Value[SYSDEV_PROP_VALUE_MAX];
} SysDeviceProp;

typedef struct SysDevice {
    ListHead Node;              // in the global list
    ListHead SiblingNode;       // in the list of the parent's children

    UINT32 Id;                  // unique ID
    CHAR   Name[SYSDEV_NAME_MAX];       // unique name
    CHAR   Description[SYSDEV_NAME_MAX]; // human-readable
    SysDeviceType  Type;
    SysDeviceState State;

    struct SysDevice *Parent;
    ListHead Children;          // list of children
    UINT32 ChildCount;

    KDriver *Driver;            // linked driver
    VOID    *Private;           // private driver data

    // Resources
    SysDeviceResource Resources[SYSDEV_MAX_RESOURCES];
    UINT32 ResourceCount;

    // Features
    SysDeviceProp Properties[SYSDEV_MAX_PROPS];
    UINT32 PropCount;

    UINT64 AddTick;             // when detected
} SysDevice;

// Traversing the tree
typedef VOID (*SysDeviceVisitor)(SysDevice *Dev, VOID *UserData);

VOID SysDeviceInit(VOID);

// Create a root device (usually only one "/").
SysDevice *SysDeviceCreateRoot(const CHAR *Name);

// Create a device with a parent. It is automatically added to the tree.
// The Id and AddTick are assigned automatically.
SysDevice *SysDeviceCreate(SysDevice *Parent,
                           const CHAR *Name,
                           const CHAR *Description,
                           SysDeviceType Type);

// Delete the device (and all children). Frees up memory.
// If the driver is linked, it unlinks it.
VOID SysDeviceDestroy(SysDevice *Dev);

// Find it by name (across the entire tree).
SysDevice *SysDeviceFind(const CHAR *Name);

// Find by ID.
SysDevice *SysDeviceFindById(UINT32 Id);

// Tree traversal (pre-order).
VOID SysDeviceWalk(SysDeviceVisitor Visitor, VOID *UserData);

// A crawl from a specific node.
VOID SysDeviceWalkFrom(SysDevice *Root, SysDeviceVisitor Visitor, VOID *UserData);

VOID SysDeviceSetState(SysDevice *Dev, SysDeviceState State);
const CHAR *SysDeviceStateToString(SysDeviceState State);
const CHAR *SysDeviceTypeToString(SysDeviceType Type);

// Link the driver. Returns SUCCESS if ok.
INT SysDeviceBindDriver(SysDevice *Dev, KDriver *Driver);

// Unlink the driver.
INT SysDeviceUnbindDriver(SysDevice *Dev);

KDriver *SysDeviceGetDriver(SysDevice *Dev);

// Add a resource.
INT SysDeviceAddResource(SysDevice *Dev, SysDeviceResType Type,
                         UINT64 Base, UINT64 Size);

// Find a resource by type. Returns the index or -1.
INT SysDeviceFindResource(SysDevice *Dev, SysDeviceResType Type);

const CHAR *SysDeviceResTypeToString(SysDeviceResType Type);

VOID SysDeviceSetProp(SysDevice *Dev, const CHAR *Name, const CHAR *Value);
VOID SysDeviceSetPropU32(SysDevice *Dev, const CHAR *Name, UINT32 Value);
VOID SysDeviceSetPropU64(SysDevice *Dev, const CHAR *Name, UINT64 Value);
VOID SysDeviceSetPropHex(SysDevice *Dev, const CHAR *Name, UINT64 Value);

const CHAR *SysDeviceGetProp(SysDevice *Dev, const CHAR *Name);

UINT32 SysDeviceGetCount(VOID);
SysDevice *SysDeviceGetRoot(VOID);

VOID SysDeviceGetPath(SysDevice *Dev, CHAR *Buf, USIZE BufSize);