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
    SYSDEV_TYPE_ROOT,           // корень дерева
    SYSDEV_TYPE_BUS,            // шина (PCI, USB, ISA)
    SYSDEV_TYPE_CONTROLLER,     // контроллер (IDE, SATA, USB-HC, FDC)
    SYSDEV_TYPE_STORAGE,        // диск, флешка
    SYSDEV_TYPE_NETWORK,        // сетевой адаптер
    SYSDEV_TYPE_DISPLAY,        // видеокарта
    SYSDEV_TYPE_SOUND,          // звук
    SYSDEV_TYPE_INPUT,          // клавиатура, мышь
    SYSDEV_TYPE_SERIAL,         // COM, USB-Serial
    SYSDEV_TYPE_TIMER,          // HPET, PIT, APIC timer
    SYSDEV_TYPE_INTERRUPT,      // APIC, IOAPIC, PIC
    SYSDEV_TYPE_POWER,          // ACPI, PMC
    SYSDEV_TYPE_VIRTUAL,        // виртуальное устройство (loopback, null)
} SysDeviceType;

typedef enum {
    SYSDEV_STATE_UNKNOWN = 0,
    SYSDEV_STATE_PRESENT,       // обнаружено, драйвер ещё не привязан
    SYSDEV_STATE_ENABLED,       // драйвер привязан, работает
    SYSDEV_STATE_DISABLED,      // отключено вручную
    SYSDEV_STATE_ERROR,         // ошибка инициализации
    SYSDEV_STATE_REMOVED,       // удалено (hot-plug)
} SysDeviceState;

typedef enum {
    SYSDEV_RES_IRQ = 0,         // линия IRQ / GSI / вектор
    SYSDEV_RES_MMIO,            // memory-mapped I/O
    SYSDEV_RES_IO,              // I/O ports (start/end)
    SYSDEV_RES_DMA,             // DMA channel
    SYSDEV_RES_MEM,             // память (буферы, окна)
    SYSDEV_RES_BUS,             // диапазон шины (PCI bus range)
} SysDeviceResType;

typedef struct {
    SysDeviceResType Type;

    // Для IRQ: Base = IRQ/GSI, Size = вектор
    // Для MMIO: Base = phys addr, Size = длина
    // Для IO: Base = start port, Size = кол-во портов
    // Для DMA: Base = канал, Size = 1
    // Для MEM: Base = phys addr, Size = длина
    // Для BUS: Base = start bus, Size = кол-во
    UINT64 Base;
    UINT64 Size;

    UINT32 Flags;               // зарезервировано для будущего
} SysDeviceResource;

typedef struct {
    CHAR Name[SYSDEV_PROP_NAME_MAX];
    CHAR Value[SYSDEV_PROP_VALUE_MAX];
} SysDeviceProp;

typedef struct SysDevice {
    ListHead Node;              // в глобальном списке
    ListHead SiblingNode;       // в списке детей родителя

    UINT32 Id;                  // уникальный ID
    CHAR   Name[SYSDEV_NAME_MAX];       // уникальное имя
    CHAR   Description[SYSDEV_NAME_MAX]; // человекочитаемое
    SysDeviceType  Type;
    SysDeviceState State;

    struct SysDevice *Parent;
    ListHead Children;          // список детей
    UINT32 ChildCount;

    KDriver *Driver;            // привязанный драйвер
    VOID    *Private;           // приватные данные драйвера

    // Ресурсы
    SysDeviceResource Resources[SYSDEV_MAX_RESOURCES];
    UINT32 ResourceCount;

    // Свойства
    SysDeviceProp Properties[SYSDEV_MAX_PROPS];
    UINT32 PropCount;

    UINT64 AddTick;             // когда обнаружено
} SysDevice;

// Обход дерева
typedef VOID (*SysDeviceVisitor)(SysDevice *Dev, VOID *UserData);

VOID SysDeviceInit(VOID);

// Создать корневое устройство (обычно только одно — "/").
SysDevice *SysDeviceCreateRoot(const CHAR *Name);

// Создать устройство с родителем. Автоматически добавляется в дерево.
// Id и AddTick назначаются автоматически.
SysDevice *SysDeviceCreate(SysDevice *Parent,
                           const CHAR *Name,
                           const CHAR *Description,
                           SysDeviceType Type);

// Удалить устройство (и всех детей). Освобождает память.
// Если драйвер привязан — отвязывает.
VOID SysDeviceDestroy(SysDevice *Dev);

// Найти по имени (по всему дереву).
SysDevice *SysDeviceFind(const CHAR *Name);

// Найти по ID.
SysDevice *SysDeviceFindById(UINT32 Id);

// Обход дерева (pre-order).
VOID SysDeviceWalk(SysDeviceVisitor Visitor, VOID *UserData);

// Обход от конкретного узла.
VOID SysDeviceWalkFrom(SysDevice *Root, SysDeviceVisitor Visitor, VOID *UserData);

VOID SysDeviceSetState(SysDevice *Dev, SysDeviceState State);
const CHAR *SysDeviceStateToString(SysDeviceState State);
const CHAR *SysDeviceTypeToString(SysDeviceType Type);

// Привязать драйвер. Возвращает SUCCESS если ок.
INT SysDeviceBindDriver(SysDevice *Dev, KDriver *Driver);

// Отвязать драйвер.
INT SysDeviceUnbindDriver(SysDevice *Dev);

KDriver *SysDeviceGetDriver(SysDevice *Dev);

// Добавить ресурс.
INT SysDeviceAddResource(SysDevice *Dev, SysDeviceResType Type,
                         UINT64 Base, UINT64 Size);

// Найти ресурс по типу. Возвращает индекс или -1.
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