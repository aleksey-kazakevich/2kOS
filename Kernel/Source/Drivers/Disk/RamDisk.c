#include <Drivers/Disk/RamDisk.h>
#include <Mem/Allocator.h>
#include <Mem/PhysAlloc.h>
#include <Mem/Paging.h>
#include <Lib/String.h>
#include <Basecon.h>
#include <KDriver.h>
#include <Return.h>

#define RAMDISK_SECTOR_SIZE 512

typedef struct {
    VOID  *VirtAddr;        // виртуальный адрес (HHDM)
    UINT64 PhysAddr;        // физический адрес начала (для справки)
    UINT64 SizeBytes;
    UINT64 SectorCount;
    UINT32 Pages;           // сколько страниц выделено
    UINT32 RefCount;
} RamDiskData;

static RamDiskData GRamDisks[RAMDISK_MAX_COUNT];

/* ============================================================================
 * Read / Write — работают с HHDM-адресом напрямую
 * ============================================================================ */

static INT RamDiskRead(Disk *Self, UINT64 Lba, UINT32 Count, VOID *Buf) {
    if (!Self || !Buf) return DEVICE_INVALID;

    RamDiskData *Rd = (RamDiskData *)Self->DriverData;
    if (!Rd || !Rd->VirtAddr) return DEVICE_INVALID;

    if (Lba + Count > Rd->SectorCount) return INCORRECT_VALUE;

    UINT64 Offset = Lba * RAMDISK_SECTOR_SIZE;
    UINT64 Bytes = (UINT64)Count * RAMDISK_SECTOR_SIZE;

    MemCpy(Buf, (UINT8 *)Rd->VirtAddr + Offset, Bytes);
    return SUCCESS;
}

static INT RamDiskWrite(Disk *Self, UINT64 Lba, UINT32 Count, const VOID *Buf) {
    if (!Self || !Buf) return DEVICE_INVALID;

    RamDiskData *Rd = (RamDiskData *)Self->DriverData;
    if (!Rd || !Rd->VirtAddr) return DEVICE_INVALID;

    if (Lba + Count > Rd->SectorCount) return INCORRECT_VALUE;

    UINT64 Offset = Lba * RAMDISK_SECTOR_SIZE;
    UINT64 Bytes = (UINT64)Count * RAMDISK_SECTOR_SIZE;

    MemCpy((UINT8 *)Rd->VirtAddr + Offset, Buf, Bytes);
    return SUCCESS;
}

static INT RamDiskFlush(Disk *Self) {
    (VOID)Self;
    return SUCCESS;
}

/* ============================================================================
 * Create / Destroy
 * ============================================================================ */

Disk *RamDiskCreate(const CHAR *Name, UINT64 SizeBytes) {
    if (!Name || Name[0] == '\0') return NULLPTR;
    if (SizeBytes == 0) return NULLPTR;

    // Выравниваем по странице (PAGE_SIZE)
    SizeBytes = (SizeBytes + PAGE_SIZE - 1) & ~(UINT64)(PAGE_SIZE - 1);
    UINT64 Sectors = SizeBytes / RAMDISK_SECTOR_SIZE;
    UINT32 Pages = (UINT32)(SizeBytes / PAGE_SIZE);

    // Ищем свободный слот
    INT Slot = -1;
    for (INT I = 0; I < RAMDISK_MAX_COUNT; I++) {
        if (!GRamDisks[I].VirtAddr) {
            Slot = I;
            break;
        }
    }
    if (Slot < 0) {
        BaseconPrintf(BASECON_TYPE_ERROR, "ramdisk: no free slots\n");
        return NULLPTR;
    }

    // ═══════════════════════════════════════════════════════════════
    // Выделяем через PMM — непрерывные физические страницы
    // PhysAllocAllocateRange возвращает виртуальный адрес (HHDM)
    // и уже обнуляет память (MemSet внутри).
    // ═══════════════════════════════════════════════════════════════
    VOID *Virt = PhysAllocAllocateRange(PhysAllocGet(), Pages);
    if (!Virt) {
        BaseconPrintf(BASECON_TYPE_ERROR,
                      "ramdisk: cannot allocate %u pages (%llu MB)\n",
                      Pages, SizeBytes / (1024 * 1024));
        return NULLPTR;
    }

    UINT64 Phys = VirtToPhys((UINT64)Virt);

    // Заполняем слот
    GRamDisks[Slot].VirtAddr = Virt;
    GRamDisks[Slot].PhysAddr = Phys;
    GRamDisks[Slot].SizeBytes = SizeBytes;
    GRamDisks[Slot].SectorCount = Sectors;
    GRamDisks[Slot].Pages = Pages;
    GRamDisks[Slot].RefCount = 1;

    // Создаём Disk
    Disk *D = (Disk *)MemoryAllocate(sizeof(Disk));
    if (!D) {
        PhysAllocFreeRange(PhysAllocGet(), Virt, Pages);
        GRamDisks[Slot].VirtAddr = NULLPTR;
        return NULLPTR;
    }
    MemSet(D, 0, sizeof(Disk));

    StrnCpy(D->Name, Name, sizeof(D->Name) - 1);
    D->Type = DISK_TYPE_RAM;
    D->SectorSize = RAMDISK_SECTOR_SIZE;
    D->SectorCount = Sectors;
    D->TotalSize = SizeBytes;
    D->DriverData = &GRamDisks[Slot];
    D->Initialized = TRUE;
    D->Read = RamDiskRead;
    D->Write = RamDiskWrite;
    D->Flush = RamDiskFlush;

    ListInit(&D->Partitions);
    ListInit(&D->LogicalPartitions);

    // Регистрируем
    INT Ret = DiskMgrRegisterDisk(D);
    if (Ret != SUCCESS) {
        PhysAllocFreeRange(PhysAllocGet(), Virt, Pages);
        GRamDisks[Slot].VirtAddr = NULLPTR;
        MemoryFree(D);
        BaseconPrintf(BASECON_TYPE_ERROR, "ramdisk: register failed (%d)\n", Ret);
        return NULLPTR;
    }

    BaseconPrintf(BASECON_TYPE_SUCCESS,
                  "ramdisk: created %s (%llu MB, %u pages)\n",
                  Name, SizeBytes / (1024 * 1024), Pages);
    return D;
}

VOID RamDiskDestroy(Disk *RamDisk) {
    if (!RamDisk) return;

    RamDiskData *Rd = (RamDiskData *)RamDisk->DriverData;
    if (!Rd) return;

    if (Rd->RefCount > 1) {
        Rd->RefCount--;
        return;
    }

    if (Rd->VirtAddr) {
        PhysAllocFreeRange(PhysAllocGet(), Rd->VirtAddr, Rd->Pages);
        Rd->VirtAddr = NULLPTR;
    }
    Rd->SizeBytes = 0;
    Rd->SectorCount = 0;
    Rd->Pages = 0;

    MemoryFree(RamDisk);
}

VOID RamDiskInit(VOID) {
    MemSet(GRamDisks, 0, sizeof(GRamDisks));
    KDriverRegister(KDriverGenerateStruct("RamDisk", DCL2, TRUE, NULLPTR, NULLPTR));
}