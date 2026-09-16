#pragma once

#include <Drivers/Disk/DiskMgr.h>
#include <Types.h>

#define RAMDISK_MAX_COUNT  8
#define RAMDISK_NAME_MAX   32

// Создать RAMDisk размером SizeBytes.
// Возвращает указатель на Disk, или NULLPTR при ошибке.
Disk *RamDiskCreate(const CHAR *Name, UINT64 SizeBytes);

// Уничтожить RAMDisk (освободить память).
VOID RamDiskDestroy(Disk *RamDisk);

// Инициализация подсистемы RAMDisk.
VOID RamDiskInit(VOID);